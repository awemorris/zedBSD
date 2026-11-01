/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client side of the NV12, query and video report host check:
 * command frames written with libvulkan's own wire and record encoders
 * (userland/desktop/libvulkan wire.c and codec.c, linked unchanged) in
 * the forms libvulkan's instance.c, external-properties.c, memory.c,
 * resources*.c, descriptors.c, commands*.c, query.c and queue.c send, and
 * the checks of the replies the executor wrote.  The first frame (the
 * instance, the device and its queue) is the executor host check's own
 * (plan/tools/powervr/p009/frame-host.c, frame_setup).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "frame-host.h"
#include "nv12-frames.h"

/* The places of the reply region: the replies from 0, the trailer in the last 20 bytes. */
#define REPLY_TRAILER_BYTES	20U

/* The identities frame_setup gives the physical device, the device and the queue. */
#define ID_PHYSICAL		2U
#define ID_DEVICE		3U
#define ID_QUEUE		4U

/* The other identities of the frames. */
#define ID_SAMPLER		110U
#define ID_SET_LAYOUT		111U
#define ID_DESCRIPTOR_POOL	112U
#define ID_READBACK		113U
#define ID_READBACK_MEMORY	114U
#define ID_COMMAND_POOL		115U
#define ID_COMMAND		116U
#define ID_BAD_COMMAND		117U
#define ID_FENCE		118U
#define ID_OCCLUSION_POOL	119U
#define ID_REFUSED_FIRST	120U
#define ID_REFUSED_VIEW_FIRST	130U

/* The NV12 image's planes: the luma plane's bytes and the chroma plane's offset (both 4 KiB multiples here). */
#define LUMA_BYTES		((uint64_t)NV12_WIDTH * 1088U)
#define CHROMA_OFFSET		LUMA_BYTES
#define CHROMA_BYTES		((uint64_t)NV12_WIDTH * 544U)
#define NV12_BYTES		(CHROMA_OFFSET + CHROMA_BYTES)

/* The readback buffer: the luma plane's texels, then the chroma plane's, tightly. */
#define READBACK_CHROMA		((uint64_t)NV12_WIDTH * NV12_HEIGHT)
#define READBACK_BYTES		(READBACK_CHROMA + (uint64_t)NV12_WIDTH * NV12_HEIGHT / 2U)

/* The refused image creates: sampled without the extended usage flag, an odd width, drawn into, and decoded into without a decoder. */
#define REFUSED_IMAGES		4U

/* The refused views: the whole image's format, a plane in the other plane's format, the colour aspect. */
#define REFUSED_VIEWS		3U

/* Local objects whose pointers are the handles libvulkan's encoders turn into wire identities. */
static struct vulkan_object objects[256];

/* The checks' counts. */
static int failures;
static int passes;

/* The handle of a wire identity. */
static uint64_t
handle(
	uint64_t identity)
{
	objects[identity].wire_id = identity;
	return (uint64_t)(uintptr_t)&objects[identity];
}

/* One check of a reply. */
static void
check(
	int condition,
	const char *what)
{
	if (condition) {
		passes++;
		return;
	}

	failures++;
	printf("FAIL: nv12 frame: %s\n", what);
}

/* Reports the frames' check counts. */
void
nv12_frame_counts(
	int *passed,
	int *failed)
{
	*passed = passes;
	*failed = failures;
}

/* Appends one command's bytes to the frame. */
static void
append(
	struct vulkan_writer *frame,
	struct vulkan_writer *command)
{
	vulkan_write_bytes(frame, command->data, command->bytes);
	vulkan_writer_finish(command);
}

/* Starts a frame with its reply region. */
static void
frame_begin(
	struct vulkan_writer *frame,
	uint32_t reply_blob)
{
	vulkan_writer_init(frame);
	vulkan_write_u32(frame, GPU_OP_SET_REPLY_STREAM);
	vulkan_write_u32(frame, 0);
	vulkan_write_u64(frame, 1);
	vulkan_write_u32(frame, reply_blob);
	vulkan_write_u64(frame, 0);
	vulkan_write_u64(frame, FRAME_REPLY_BYTES);
}

/* Ends a frame with the decoder's trailer, and hands its bytes over. */
static size_t
frame_end(
	struct vulkan_writer *frame,
	uint8_t **bytes)
{
	vulkan_write_u32(frame, GPU_OP_SEEK_REPLY_STREAM);
	vulkan_write_u32(frame, 0);
	vulkan_write_u64(frame, FRAME_REPLY_BYTES - REPLY_TRAILER_BYTES);
	vulkan_write_u32(frame, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	vulkan_write_u32(frame, 1);
	vulkan_write_u64(frame, 1);
	check(frame->error == VK_SUCCESS, "frame encodes");
	*bytes = frame->data;
	return frame->bytes;
}

/* A create's tail: [allocator][present][identity]. */
static void
create_tail(
	struct vulkan_writer *writer,
	uint64_t identity)
{
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, identity);
}

/* Starts a reader over a frame's replies. */
static void
reader_begin(
	struct vulkan_reader *reader,
	uint8_t *reply)
{
	vulkan_reader_init(reader, NULL, 0);
	reader->data = reply;
	reader->bytes = FRAME_REPLY_BYTES - REPLY_TRAILER_BYTES;
}

/* Reads a reply's echoed opcode and checks it. */
static void
expect_opcode(
	struct vulkan_reader *reader,
	uint32_t opcode,
	const char *what)
{
	uint32_t echoed;

	echoed = vulkan_read_u32(reader);
	check(echoed == opcode, what);
}

/* Checks a create's reply: [opcode][result][present][identity]. */
static void
expect_created(
	struct vulkan_reader *reader,
	uint32_t opcode,
	uint64_t identity,
	const char *what)
{
	uint32_t result;
	uint64_t present;
	uint64_t answered;

	expect_opcode(reader, opcode, what);
	result = vulkan_read_u32(reader);
	present = vulkan_read_u64(reader);
	answered = vulkan_read_u64(reader);
	check(result == VK_SUCCESS && present == 1 && answered == identity, what);
}

/* Checks a refused create's reply: [opcode][result], a failure. */
static void
expect_refused(
	struct vulkan_reader *reader,
	uint32_t opcode,
	const char *what)
{
	uint32_t result;

	expect_opcode(reader, opcode, what);
	result = vulkan_read_u32(reader);
	check(result != VK_SUCCESS, what);
}

/* Checks a result-only reply: [opcode][result]. */
static void
expect_result(
	struct vulkan_reader *reader,
	uint32_t opcode,
	uint32_t expected,
	const char *what)
{
	uint32_t result;

	expect_opcode(reader, opcode, what);
	result = vulkan_read_u32(reader);
	check(result == expected, what);
}

/* Appends a vkGetPhysicalDeviceImageFormatProperties of NV12 2D in a tiling, usage and flags. */
static void
image_format(
	struct vulkan_writer *frame,
	uint32_t format,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags)
{
	struct vulkan_writer w;

	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES);
	vulkan_write_u64(&w, ID_PHYSICAL);
	vulkan_write_u32(&w, format);
	vulkan_write_u32(&w, VK_IMAGE_TYPE_2D);
	vulkan_write_u32(&w, tiling);
	vulkan_write_u32(&w, usage);
	vulkan_write_u32(&w, flags);
	vulkan_write_u64(&w, 1);
	append(frame, &w);
}

/* Appends a vkGetPhysicalDeviceImageFormatProperties2 with an external handle type, as external_image_properties sends it. */
static void
image_format_external(
	struct vulkan_writer *frame,
	uint32_t handle_type,
	uint32_t format,
	uint32_t usage,
	uint32_t flags)
{
	struct vulkan_writer w;

	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2);
	vulkan_write_u64(&w, ID_PHYSICAL);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
	vulkan_write_u64(&w, 0);
	vulkan_write_u32(&w, handle_type);
	vulkan_write_u32(&w, format);
	vulkan_write_u32(&w, VK_IMAGE_TYPE_2D);
	vulkan_write_u32(&w, VK_IMAGE_TILING_OPTIMAL);
	vulkan_write_u32(&w, usage);
	vulkan_write_u32(&w, flags);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	vulkan_write_u64(&w, 0);
	append(frame, &w);
}

/* Appends a vkGetPhysicalDeviceExternalBufferProperties of a handle type, as external_buffer_properties sends it. */
static void
buffer_external(
	struct vulkan_writer *frame,
	uint32_t handle_type)
{
	struct vulkan_writer w;

	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES);
	vulkan_write_u64(&w, ID_PHYSICAL);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO);
	vulkan_write_u64(&w, 0);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	vulkan_write_u32(&w, handle_type);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
	vulkan_write_u64(&w, 0);
	append(frame, &w);
}

/*
 * The report frame: NV12's format features, its image queries (the plane
 * views' flags, sampling without them, a decode's usage), the external
 * queries of an opaque fd and of a handle type the node does not share
 * by, the queue family and its codec operations.
 */
size_t
nv12_frame_reports(
	uint32_t reply_blob,
	uint8_t **bytes)
{
	struct vulkan_writer frame;
	struct vulkan_writer w;

	frame_begin(&frame, reply_blob);

	/* NV12's features. */
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES);
	vulkan_write_u64(&w, ID_PHYSICAL);
	vulkan_write_u32(&w, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	vulkan_write_u64(&w, 1);
	append(&frame, &w);

	/* NV12's image queries: Video Player's import, sampled without the flags, copied only, decoded into. */
	image_format(&frame, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT,
		     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
	image_format(&frame, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0);
	image_format(&frame, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
	image_format(&frame, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_SAMPLED_BIT,
		     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);

	/* A colour format still refuses flags. */
	image_format(&frame, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);

	/* The external queries: an opaque fd image of each kind, an image of a type the node does not share by, buffers. */
	image_format_external(&frame, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, 0);
	image_format_external(&frame, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
			      VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
	image_format_external(&frame, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, 0);
	buffer_external(&frame, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
	buffer_external(&frame, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);

	/* The queue family, then its codec operations. */
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES);
	vulkan_write_u64(&w, ID_PHYSICAL);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, 1);
	vulkan_write_u64(&w, 1);
	append(&frame, &w);
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES);
	vulkan_write_u64(&w, ID_PHYSICAL);
	vulkan_write_u32(&w, 1);
	append(&frame, &w);

	return frame_end(&frame, bytes);
}

/* Checks an image query's reply: [opcode][result][present][properties]; a success's limits are one level and layer of NV12. */
static void
expect_image_format(
	struct vulkan_reader *reader,
	uint32_t expected,
	int planar,
	const char *what)
{
	VkImageFormatProperties properties;
	uint32_t result;

	expect_opcode(reader, GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES, what);
	result = vulkan_read_u32(reader);
	check(vulkan_reply_pointer(reader), what);
	memset(&properties, 0, sizeof(properties));
	vulkan_decode_VkImageFormatProperties(reader, &properties);
	check(result == expected, what);
	if (result == VK_SUCCESS && planar)
		check(properties.maxMipLevels == 1 && properties.maxArrayLayers == 1 && properties.maxExtent.width == 4096U, what);
}

/* Checks an external image query's reply: the result, the headers, the external memory properties. */
static void
expect_image_external(
	struct vulkan_reader *reader,
	uint32_t expected_result,
	uint32_t expected_features,
	uint32_t expected_types,
	const char *what)
{
	VkImageFormatProperties properties;
	uint32_t result;
	uint32_t features;
	uint32_t exported;
	uint32_t compatible;

	expect_opcode(reader, GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2, what);
	result = vulkan_read_u32(reader);
	check(vulkan_read_u64(reader) == 1, what);
	check(vulkan_read_u32(reader) == VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 && vulkan_read_u64(reader) == 1, what);
	check(vulkan_read_u32(reader) == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES && vulkan_read_u64(reader) == 0, what);
	features = vulkan_read_u32(reader);
	exported = vulkan_read_u32(reader);
	compatible = vulkan_read_u32(reader);
	vulkan_decode_VkImageFormatProperties(reader, &properties);
	check(result == expected_result && features == expected_features && exported == expected_types && compatible == expected_types, what);
}

/* Checks an external buffer query's reply: the header and the external memory properties. */
static void
expect_buffer_external(
	struct vulkan_reader *reader,
	uint32_t expected_features,
	uint32_t expected_types,
	const char *what)
{
	uint32_t features;
	uint32_t exported;
	uint32_t compatible;

	expect_opcode(reader, GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES, what);
	check(vulkan_read_u64(reader) == 1, what);
	check(vulkan_read_u32(reader) == VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES && vulkan_read_u64(reader) == 0, what);
	features = vulkan_read_u32(reader);
	exported = vulkan_read_u32(reader);
	compatible = vulkan_read_u32(reader);
	check(features == expected_features && exported == expected_types && compatible == expected_types, what);
}

/* Checks the report frame's replies against the session's video (or none). */
void
nv12_replies_reports(
	uint8_t *reply,
	const struct nv12_expect *expect)
{
	VkFormatProperties format;
	VkQueueFamilyProperties family;
	struct vulkan_reader reader;
	uint32_t shared;
	uint32_t video;

	reader_begin(&reader, reply);
	shared = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;

	/* NV12 is copied out on either tiling, decoded into only with the decoder, and never sampled whole. */
	expect_opcode(&reader, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES, "nv12 format");
	check(vulkan_reply_pointer(&reader), "nv12 format present");
	memset(&format, 0, sizeof(format));
	vulkan_decode_VkFormatProperties(&reader, &format);
	check(format.optimalTilingFeatures == expect->nv12_optimal_features, "nv12 optimal features");
	check(format.linearTilingFeatures == expect->nv12_optimal_features, "nv12 linear features");
	check((format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0, "nv12 is not sampled whole");

	/* The image queries. */
	expect_image_format(&reader, VK_SUCCESS, 1, "nv12 sampled with the plane views' flags (Video Player's import)");
	expect_image_format(&reader, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED, 1, "nv12 sampled without the extended usage flag");
	expect_image_format(&reader, VK_SUCCESS, 1, "nv12 linear, copied out");
	video = VK_SUCCESS;
	if (!expect->video_usage_allowed)
		video = (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	expect_image_format(&reader, video, 1, "nv12 decoded into only with the decoder");
	expect_image_format(&reader, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED, 0, "a colour format with create flags");

	/* The external queries. */
	expect_image_external(&reader, VK_SUCCESS, shared, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "an opaque fd colour image is exported and imported");
	expect_image_external(&reader, VK_SUCCESS, shared, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "an opaque fd nv12 image is exported and imported");
	expect_image_external(&reader, VK_SUCCESS, 0, 0, "a handle type the node does not share by has nothing");
	expect_buffer_external(&reader, shared, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "an opaque fd buffer");
	expect_buffer_external(&reader, 0, 0, "a buffer of another handle type has nothing");

	/* The family's flags, then its codec operations. */
	expect_opcode(&reader, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, "family");
	check(vulkan_read_u64(&reader) == 1 && vulkan_read_u32(&reader) == 1 && vulkan_read_u64(&reader) == 1, "one family in the array");
	memset(&family, 0, sizeof(family));
	vulkan_decode_VkQueueFamilyProperties(&reader, &family);
	check(family.queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | expect->queue_flags), "the family's flags");
	expect_opcode(&reader, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES, "family video");
	check(vulkan_read_u64(&reader) == 1 && vulkan_read_u32(&reader) == 1, "one family's codec operations");
	check(vulkan_read_u32(&reader) == expect->codecs, "the family's codec operations");
	check(reader.error == VK_SUCCESS, "report replies read");
}

/* Appends a vkCreateImage of NV12 (or another format) with flags, usage, tiling and, when external, the opaque fd declaration. */
static void
create_image(
	struct vulkan_writer *frame,
	uint64_t identity,
	uint32_t format,
	uint32_t width,
	uint32_t height,
	uint32_t tiling,
	uint32_t usage,
	uint32_t flags,
	int external)
{
	VkExternalMemoryImageCreateInfo declaration;
	VkImageCreateInfo image;
	struct vulkan_writer w;

	memset(&declaration, 0, sizeof(declaration));
	declaration.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	declaration.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	if (external)
		image.pNext = &declaration;
	image.flags = flags;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = format;
	image.extent.width = width;
	image.extent.height = height;
	image.extent.depth = 1;
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = tiling;
	image.usage = usage;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_IMAGE);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkImageCreateInfo(&w, &image);
	create_tail(&w, identity);
	append(frame, &w);
}

/*
 * The image frame: the decoder's NV12 image (sampled, copied out, the
 * plane views' flags) and Video Player's import of it (sampled only,
 * external), the refused creates, and the image's requirements.
 */
size_t
nv12_frame_image(
	uint32_t reply_blob,
	uint8_t **bytes)
{
	struct vulkan_writer frame;
	struct vulkan_writer w;
	uint32_t views;

	frame_begin(&frame, reply_blob);
	views = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

	/* The two images, then the refusals. */
	create_image(&frame, NV12_ID_IMAGE, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, NV12_WIDTH, NV12_HEIGHT, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, views, 0);
	create_image(&frame, NV12_ID_IMPORT_IMAGE, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, NV12_WIDTH, NV12_HEIGHT, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_SAMPLED_BIT, views, 1);
	create_image(&frame, ID_REFUSED_FIRST, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, NV12_WIDTH, NV12_HEIGHT, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, 0);
	create_image(&frame, ID_REFUSED_FIRST + 1U, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, NV12_WIDTH - 1U, NV12_HEIGHT, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 0);
	create_image(&frame, ID_REFUSED_FIRST + 2U, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, NV12_WIDTH, NV12_HEIGHT, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, views, 0);
	create_image(&frame, ID_REFUSED_FIRST + 3U, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, NV12_WIDTH, NV12_HEIGHT, VK_IMAGE_TILING_OPTIMAL,
		     VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, 0, 0);

	/* The decoder's image's requirements, and the import's. */
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_encode_handle(&w, handle(NV12_ID_IMAGE));
	vulkan_write_u64(&w, 1);
	append(&frame, &w);
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_encode_handle(&w, handle(NV12_ID_IMPORT_IMAGE));
	vulkan_write_u64(&w, 1);
	append(&frame, &w);

	return frame_end(&frame, bytes);
}

/* Checks the image frame's replies; *image_bytes gets the decoder's image's size. */
void
nv12_replies_image(
	uint8_t *reply,
	uint64_t *image_bytes)
{
	VkMemoryRequirements requirements;
	VkMemoryRequirements imported;
	struct vulkan_reader reader;
	uint32_t index;

	reader_begin(&reader, reply);

	/* The two images, then the refusals. */
	expect_created(&reader, GPU_OP_CREATE_IMAGE, NV12_ID_IMAGE, "the decoder's nv12 image");
	expect_created(&reader, GPU_OP_CREATE_IMAGE, NV12_ID_IMPORT_IMAGE, "Video Player's nv12 import");
	for (index = 0; index < REFUSED_IMAGES; index++)
		expect_refused(&reader, GPU_OP_CREATE_IMAGE, "a refused nv12 create");

	/* The requirements: both planes, whole pages, either type, the same for the import. */
	expect_opcode(&reader, GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS, "nv12 requirements");
	check(vulkan_reply_pointer(&reader), "nv12 requirements present");
	memset(&requirements, 0, sizeof(requirements));
	vulkan_decode_VkMemoryRequirements(&reader, &requirements);
	check(requirements.size == NV12_BYTES && requirements.alignment == 4096U && requirements.memoryTypeBits == 3U, "nv12 requirements: both planes");
	expect_opcode(&reader, GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS, "import requirements");
	check(vulkan_reply_pointer(&reader), "import requirements present");
	memset(&imported, 0, sizeof(imported));
	vulkan_decode_VkMemoryRequirements(&reader, &imported);
	check(imported.size == requirements.size, "the import lies as the decoder's image does");
	check(reader.error == VK_SUCCESS, "image replies read");
	*image_bytes = requirements.size;
}

/* Appends a vkAllocateMemory of host visible memory. */
static void
allocate_memory(
	struct vulkan_writer *frame,
	uint64_t identity,
	uint64_t bytes)
{
	struct vulkan_writer w;

	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_ALLOCATE_MEMORY);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	vulkan_write_u64(&w, 0);
	vulkan_write_u64(&w, bytes);
	vulkan_write_u32(&w, 1);
	create_tail(&w, identity);
	append(frame, &w);
}

/* Appends a vkGetImageSubresourceLayout of the NV12 image's aspect. */
static void
subresource_layout(
	struct vulkan_writer *frame,
	uint32_t aspect)
{
	VkImageSubresource subresource;
	struct vulkan_writer w;

	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = aspect;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_encode_handle(&w, handle(NV12_ID_IMAGE));
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkImageSubresource(&w, &subresource);
	vulkan_write_u64(&w, 1);
	append(frame, &w);
}

/* Appends a vkCreateImageView of the NV12 image in a format and aspect. */
static void
create_view(
	struct vulkan_writer *frame,
	uint64_t identity,
	uint32_t format,
	uint32_t aspect)
{
	VkImageViewCreateInfo view;
	struct vulkan_writer w;

	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = (VkImage)(uintptr_t)handle(NV12_ID_IMAGE);
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange.aspectMask = aspect;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkImageViewCreateInfo(&w, &view);
	create_tail(&w, identity);
	append(frame, &w);
}

/*
 * The memory frame: the NV12 image's memory and binding, its planes'
 * layouts, the two plane views and the refused ones, a sampler, and a
 * set of two combined image samplers written with the planes' views.
 */
size_t
nv12_frame_memory(
	uint32_t reply_blob,
	uint64_t image_bytes,
	uint8_t **bytes)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout set_layout_handle;
	struct vulkan_writer frame;
	struct vulkan_writer w;
	uint32_t index;

	frame_begin(&frame, reply_blob);

	/* The memory and the binding. */
	allocate_memory(&frame, NV12_ID_MEMORY, image_bytes);
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_BIND_IMAGE_MEMORY);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_encode_handle(&w, handle(NV12_ID_IMAGE));
	vulkan_encode_handle(&w, handle(NV12_ID_MEMORY));
	vulkan_write_u64(&w, 0);
	append(&frame, &w);

	/* Each plane's layout, and the colour aspect's (none). */
	subresource_layout(&frame, VK_IMAGE_ASPECT_PLANE_0_BIT);
	subresource_layout(&frame, VK_IMAGE_ASPECT_PLANE_1_BIT);
	subresource_layout(&frame, VK_IMAGE_ASPECT_COLOR_BIT);

	/* The planes' views, then the refusals. */
	create_view(&frame, NV12_ID_LUMA, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT);
	create_view(&frame, NV12_ID_CHROMA, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_PLANE_1_BIT);
	create_view(&frame, ID_REFUSED_VIEW_FIRST, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
	create_view(&frame, ID_REFUSED_VIEW_FIRST + 1U, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_PLANE_1_BIT);
	create_view(&frame, ID_REFUSED_VIEW_FIRST + 2U, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

	/* A sampler. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_SAMPLER);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkSamplerCreateInfo(&w, &sampler);
	create_tail(&w, ID_SAMPLER);
	append(&frame, &w);

	/* Video Player's set: two combined image samplers for the fragment stage. */
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < 2U; index++) {
		bindings[index].binding = index;
		bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 2;
	set_layout.pBindings = bindings;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_pointer(&w, &set_layout);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&w, &set_layout);
	create_tail(&w, ID_SET_LAYOUT);
	append(&frame, &w);
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 2;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &pool_size;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_DESCRIPTOR_POOL);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_pointer(&w, &pool);
	vulkan_encode_VkDescriptorPoolCreateInfo(&w, &pool);
	create_tail(&w, ID_DESCRIPTOR_POOL);
	append(&frame, &w);
	set_layout_handle = (VkDescriptorSetLayout)(uintptr_t)handle(ID_SET_LAYOUT);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = (VkDescriptorPool)(uintptr_t)handle(ID_DESCRIPTOR_POOL);
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &set_layout_handle;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_pointer(&w, &allocate);
	vulkan_encode_VkDescriptorSetAllocateInfo(&w, &allocate);
	vulkan_write_u64(&w, 1);
	vulkan_encode_handle(&w, handle(NV12_ID_SET));
	append(&frame, &w);

	/* The two writes, the luma plane at binding 0 and the chroma plane at binding 1. */
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u32(&w, 2);
	vulkan_write_u64(&w, 2);
	for (index = 0; index < 2U; index++) {
		vulkan_write_u32(&w, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
		vulkan_write_u64(&w, 0);
		vulkan_write_u64(&w, NV12_ID_SET);
		vulkan_write_u32(&w, index);
		vulkan_write_u32(&w, 0);
		vulkan_write_u32(&w, 1);
		vulkan_write_u32(&w, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		vulkan_write_u64(&w, 1);
		vulkan_write_u64(&w, ID_SAMPLER);
		vulkan_write_u64(&w, index == 0U ? NV12_ID_LUMA : NV12_ID_CHROMA);
		vulkan_write_u32(&w, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vulkan_write_u64(&w, 0);
		vulkan_write_u64(&w, 0);
	}
	vulkan_write_u32(&w, 0);
	vulkan_write_u64(&w, 0);
	append(&frame, &w);

	return frame_end(&frame, bytes);
}

/* Checks a subresource layout's reply against an offset, a size and a row. */
static void
expect_layout(
	struct vulkan_reader *reader,
	uint64_t offset,
	uint64_t size,
	uint64_t row,
	const char *what)
{
	VkSubresourceLayout layout;

	expect_opcode(reader, GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT, what);
	check(vulkan_reply_pointer(reader), what);
	memset(&layout, 0, sizeof(layout));
	vulkan_decode_VkSubresourceLayout(reader, &layout);
	check(layout.offset == offset && layout.size == size && layout.rowPitch == row, what);
}

/* Checks the memory frame's replies. */
void
nv12_replies_memory(
	uint8_t *reply)
{
	struct vulkan_reader reader;
	uint32_t index;

	reader_begin(&reader, reply);
	expect_created(&reader, GPU_OP_ALLOCATE_MEMORY, NV12_ID_MEMORY, "nv12 memory");
	expect_result(&reader, GPU_OP_BIND_IMAGE_MEMORY, VK_SUCCESS, "nv12 bound");
	expect_layout(&reader, 0, LUMA_BYTES, NV12_WIDTH, "the luma plane's layout");
	expect_layout(&reader, CHROMA_OFFSET, CHROMA_BYTES, NV12_WIDTH, "the chroma plane's layout, the same rows");
	expect_layout(&reader, 0, 0, 0, "nv12 has no colour subresource");
	expect_created(&reader, GPU_OP_CREATE_IMAGE_VIEW, NV12_ID_LUMA, "the luma plane's R8 view");
	expect_created(&reader, GPU_OP_CREATE_IMAGE_VIEW, NV12_ID_CHROMA, "the chroma plane's R8G8 view");
	for (index = 0; index < REFUSED_VIEWS; index++)
		expect_refused(&reader, GPU_OP_CREATE_IMAGE_VIEW, "a refused nv12 view");
	expect_created(&reader, GPU_OP_CREATE_SAMPLER, ID_SAMPLER, "sampler");
	expect_created(&reader, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT, ID_SET_LAYOUT, "set layout");
	expect_created(&reader, GPU_OP_CREATE_DESCRIPTOR_POOL, ID_DESCRIPTOR_POOL, "descriptor pool");
	expect_opcode(&reader, GPU_OP_ALLOCATE_DESCRIPTOR_SETS, "set");
	check(vulkan_read_u32(&reader) == VK_SUCCESS && vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == NV12_ID_SET, "set allocated");
	expect_opcode(&reader, GPU_OP_UPDATE_DESCRIPTOR_SETS, "set written");
	check(reader.error == VK_SUCCESS, "memory replies read");
}

/* Appends one recorded command's header: [opcode][0][command buffer]. */
static void
record_begin(
	struct vulkan_writer *w,
	uint32_t opcode,
	uint64_t command)
{
	vulkan_writer_init(w);
	w->bytes = 0;
	vulkan_write_u32(w, opcode);
	vulkan_write_u32(w, 0);
	vulkan_write_u64(w, command);
}

/* Appends a vkCreateQueryPool of a type and count. */
static void
create_query_pool(
	struct vulkan_writer *frame,
	uint64_t identity,
	uint32_t type,
	uint32_t count)
{
	struct vulkan_writer w;

	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_QUERY_POOL);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
	vulkan_write_u64(&w, 0);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, type);
	vulkan_write_u32(&w, count);
	vulkan_write_u32(&w, 0);
	create_tail(&w, identity);
	append(frame, &w);
}

/* Appends a vkAllocateCommandBuffers of one and its vkBeginCommandBuffer. */
static void
command_begin(
	struct vulkan_writer *frame,
	uint64_t command)
{
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	struct vulkan_writer w;

	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = (VkCommandPool)(uintptr_t)handle(ID_COMMAND_POOL);
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&w, &allocate);
	vulkan_write_u64(&w, 1);
	vulkan_write_u64(&w, command);
	append(frame, &w);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_BEGIN_COMMAND_BUFFER);
	vulkan_write_u64(&w, command);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&w, &begin);
	append(frame, &w);
}

/* Appends a vkEndCommandBuffer. */
static void
command_end(
	struct vulkan_writer *frame,
	uint64_t command)
{
	struct vulkan_writer w;

	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_END_COMMAND_BUFFER);
	vulkan_write_u64(&w, command);
	append(frame, &w);
}

/*
 * The command frame: the readback buffer and its memory, the query
 * pools (a result status one, a refused occlusion one), a command buffer
 * that resets the queries, begins and ends query 0 and copies both
 * planes out, its submission; and a command buffer that clears the NV12
 * image, whose recording fails.
 */
size_t
nv12_frame_commands(
	uint32_t reply_blob,
	uint8_t **bytes)
{
	VkBufferCreateInfo buffer;
	VkCommandPoolCreateInfo command_pool;
	VkBufferImageCopy copies[2];
	struct vulkan_writer frame;
	struct vulkan_writer w;
	uint32_t index;

	frame_begin(&frame, reply_blob);

	/* The readback buffer, its memory and binding. */
	allocate_memory(&frame, ID_READBACK_MEMORY, (READBACK_BYTES + 4095U) & ~(uint64_t)4095U);
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = READBACK_BYTES;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_BUFFER);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkBufferCreateInfo(&w, &buffer);
	create_tail(&w, ID_READBACK);
	append(&frame, &w);
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_BIND_BUFFER_MEMORY);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_encode_handle(&w, handle(ID_READBACK));
	vulkan_encode_handle(&w, handle(ID_READBACK_MEMORY));
	vulkan_write_u64(&w, 0);
	append(&frame, &w);

	/* The query pools. */
	create_query_pool(&frame, NV12_ID_POOL, VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR, 4);
	create_query_pool(&frame, ID_OCCLUSION_POOL, VK_QUERY_TYPE_OCCLUSION, 4);

	/* The command pool and the first command buffer. */
	memset(&command_pool, 0, sizeof(command_pool));
	command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_COMMAND_POOL);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_encode_VkCommandPoolCreateInfo(&w, &command_pool);
	create_tail(&w, ID_COMMAND_POOL);
	append(&frame, &w);
	command_begin(&frame, ID_COMMAND);

	/* The queries' reset, query 0's scope (no decode in it). */
	record_begin(&w, GPU_OP_CMD_RESET_QUERY_POOL, ID_COMMAND);
	vulkan_encode_handle(&w, handle(NV12_ID_POOL));
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 4);
	append(&frame, &w);
	record_begin(&w, GPU_OP_CMD_BEGIN_QUERY, ID_COMMAND);
	vulkan_encode_handle(&w, handle(NV12_ID_POOL));
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 0);
	append(&frame, &w);
	record_begin(&w, GPU_OP_CMD_END_QUERY, ID_COMMAND);
	vulkan_encode_handle(&w, handle(NV12_ID_POOL));
	vulkan_write_u32(&w, 0);
	append(&frame, &w);

	/* Both planes into the buffer, tightly: libmedia's readback. */
	memset(copies, 0, sizeof(copies));
	for (index = 0; index < 2U; index++) {
		copies[index].imageSubresource.layerCount = 1;
		copies[index].imageExtent.depth = 1;
	}
	copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	copies[0].imageExtent.width = NV12_WIDTH;
	copies[0].imageExtent.height = NV12_HEIGHT;
	copies[1].bufferOffset = READBACK_CHROMA;
	copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	copies[1].imageExtent.width = NV12_WIDTH / 2U;
	copies[1].imageExtent.height = NV12_HEIGHT / 2U;
	record_begin(&w, GPU_OP_CMD_COPY_IMAGE_TO_BUFFER, ID_COMMAND);
	vulkan_encode_handle(&w, handle(NV12_ID_IMAGE));
	vulkan_write_u32(&w, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vulkan_encode_handle(&w, handle(ID_READBACK));
	vulkan_write_u32(&w, 2);
	vulkan_write_u64(&w, 2);
	vulkan_encode_VkBufferImageCopy(&w, &copies[0]);
	vulkan_encode_VkBufferImageCopy(&w, &copies[1]);
	append(&frame, &w);
	command_end(&frame, ID_COMMAND);

	/* A fence, and the submission of the command buffer with it. */
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_CREATE_FENCE);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
	vulkan_write_u64(&w, 0);
	vulkan_write_u32(&w, 0);
	create_tail(&w, ID_FENCE);
	append(&frame, &w);
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_QUEUE_SUBMIT);
	vulkan_write_u64(&w, ID_QUEUE);
	vulkan_write_u32(&w, 1);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_STRUCTURE_TYPE_SUBMIT_INFO);
	vulkan_write_u64(&w, 0);
	vulkan_write_u32(&w, 0);
	vulkan_write_u64(&w, 0);
	vulkan_write_u64(&w, 0);
	vulkan_write_u32(&w, 1);
	vulkan_write_u64(&w, 1);
	vulkan_write_u64(&w, ID_COMMAND);
	vulkan_write_u32(&w, 0);
	vulkan_write_u64(&w, 0);
	vulkan_write_u64(&w, ID_FENCE);
	append(&frame, &w);

	/* The command buffer that clears the NV12 image: its recording fails. */
	command_begin(&frame, ID_BAD_COMMAND);
	record_begin(&w, GPU_OP_CMD_CLEAR_COLOR_IMAGE, ID_BAD_COMMAND);
	vulkan_encode_handle(&w, handle(NV12_ID_IMAGE));
	vulkan_write_u32(&w, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, 0);
	vulkan_write_u64(&w, 4);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 1);
	vulkan_write_u64(&w, 1);
	vulkan_write_u32(&w, VK_IMAGE_ASPECT_COLOR_BIT);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 1);
	vulkan_write_u32(&w, 0);
	vulkan_write_u32(&w, 1);
	append(&frame, &w);
	command_end(&frame, ID_BAD_COMMAND);

	return frame_end(&frame, bytes);
}

/* Checks the command frame's replies. */
void
nv12_replies_commands(
	uint8_t *reply)
{
	struct vulkan_reader reader;
	uint32_t result;

	reader_begin(&reader, reply);
	expect_created(&reader, GPU_OP_ALLOCATE_MEMORY, ID_READBACK_MEMORY, "readback memory");
	expect_created(&reader, GPU_OP_CREATE_BUFFER, ID_READBACK, "readback buffer");
	expect_result(&reader, GPU_OP_BIND_BUFFER_MEMORY, VK_SUCCESS, "readback bound");
	expect_created(&reader, GPU_OP_CREATE_QUERY_POOL, NV12_ID_POOL, "result status pool");
	expect_refused(&reader, GPU_OP_CREATE_QUERY_POOL, "an occlusion pool is refused");
	expect_created(&reader, GPU_OP_CREATE_COMMAND_POOL, ID_COMMAND_POOL, "command pool");
	expect_opcode(&reader, GPU_OP_ALLOCATE_COMMAND_BUFFERS, "command buffer");
	check(vulkan_read_u32(&reader) == VK_SUCCESS && vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == ID_COMMAND, "command buffer allocated");
	expect_result(&reader, GPU_OP_BEGIN_COMMAND_BUFFER, VK_SUCCESS, "begin");
	expect_result(&reader, GPU_OP_END_COMMAND_BUFFER, VK_SUCCESS, "the queries and the plane copies record");
	expect_created(&reader, GPU_OP_CREATE_FENCE, ID_FENCE, "fence");
	expect_result(&reader, GPU_OP_QUEUE_SUBMIT, VK_SUCCESS, "submit");
	expect_opcode(&reader, GPU_OP_ALLOCATE_COMMAND_BUFFERS, "bad command buffer");
	check(vulkan_read_u32(&reader) == VK_SUCCESS && vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == ID_BAD_COMMAND, "bad command buffer allocated");
	expect_result(&reader, GPU_OP_BEGIN_COMMAND_BUFFER, VK_SUCCESS, "bad begin");
	expect_opcode(&reader, GPU_OP_END_COMMAND_BUFFER, "bad end");
	result = vulkan_read_u32(&reader);
	check(result != VK_SUCCESS, "a clear of the nv12 image fails its recording");
	check(reader.error == VK_SUCCESS, "command replies read");
}

/* The results frame: one vkGetQueryPoolResults of the pool. */
size_t
nv12_frame_results(
	uint32_t reply_blob,
	uint32_t query,
	uint32_t flags,
	uint64_t stride,
	uint64_t data_bytes,
	uint8_t **bytes)
{
	struct vulkan_writer frame;
	struct vulkan_writer w;

	frame_begin(&frame, reply_blob);
	vulkan_writer_init(&w);
	vulkan_command_begin(&w, GPU_OP_GET_QUERY_POOL_RESULTS);
	vulkan_write_u64(&w, ID_DEVICE);
	vulkan_encode_handle(&w, handle(NV12_ID_POOL));
	vulkan_write_u32(&w, query);
	vulkan_write_u32(&w, 1);
	vulkan_write_u64(&w, data_bytes);
	vulkan_write_u64(&w, data_bytes);
	vulkan_write_u64(&w, stride);
	vulkan_write_u32(&w, flags);
	append(&frame, &w);
	return frame_end(&frame, bytes);
}

/* Checks the results frame's reply: the result and the record's bytes. */
void
nv12_replies_results(
	uint8_t *reply,
	uint32_t expected_result,
	const uint8_t *expected,
	size_t expected_bytes,
	const char *what)
{
	struct vulkan_reader reader;
	uint8_t data[64];
	uint32_t result;
	uint64_t bytes;

	reader_begin(&reader, reply);
	expect_opcode(&reader, GPU_OP_GET_QUERY_POOL_RESULTS, what);
	result = vulkan_read_u32(&reader);
	bytes = vulkan_read_u64(&reader);
	check(result == expected_result && bytes == expected_bytes && bytes <= sizeof(data), what);
	if (bytes <= sizeof(data)) {
		vulkan_read_bytes(&reader, data, (size_t)bytes);
		check(memcmp(data, expected, expected_bytes) == 0, what);
	}
	check(reader.error == VK_SUCCESS, what);
}
