/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercise NV12 readback through the production command decoder and
 * executor.  Reuse the WS031 session/wire fixture and its existing transfer
 * regression.  The GPU stand-in records surfaces, not rendered pixels:
 * byte-exact decode/readback still requires the i915 hardware test.
 */

#define main ws031_cmdbuf_main
#include "../../ws031/tests/i915-vk-cmdbuf-test.c"
#undef main

/* The NV12 picture occupies a different range from the transfer buffers. */
#define READBACK_IMAGE 0x320ULL
#define READBACK_IMAGE_OFFSET 16384U

static void readback_resources(void);
static void readback_copy(const VkBufferImageCopy *region, uint32_t opcode);
static uint32_t readback_run(const VkBufferImageCopy *region, uint32_t opcode);
static void test_readback(void);

/*
 * Run the existing transfer regression and the NV12 readback checks.
 */
int
main(void)
{
	/* Preserve the established single-plane copy behavior. */
	ws031_cmdbuf_main();

	/* Exercise standard plane copy admission and emitted GPU surfaces. */
	test_readback();
	printf("WS202 NV12 readback host PASS (GPU pixels unverified)\n");

	/* Succeeded: every software assertion held. */
	return 0;
}

/* Create a 66 by 34 NV12 picture and one graphics command buffer. */
static void
readback_resources(void)
{
	size_t reply_bytes;

	/* Enable the already implemented decoder in this fixture device. */
	stub_vk->video = 1;
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	stub_put32(&fixture_wire, 66U);
	stub_put32(&fixture_wire, 34U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_SAMPLE_COUNT_1_BIT);
	stub_put32(&fixture_wire, VK_IMAGE_TILING_OPTIMAL);
	stub_put32(&fixture_wire, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_UNDEFINED);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, READBACK_IMAGE);

	/* Bind the video picture to the fixture's real allocation object. */
	stub_put32(&fixture_wire, FIXTURE_BIND_IMAGE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, READBACK_IMAGE);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, READBACK_IMAGE_OFFSET);

	/* A graphics pool and primary command buffer execute the transfer. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 80U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 28U) == VK_SUCCESS);
}

/* Encode one complete standard VkBufferImageCopy, including its plane. */
static void
readback_copy(
	const VkBufferImageCopy *region,
	uint32_t opcode)
{
	/* The command's framing depends on the direction of the transfer. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	if (opcode == FIXTURE_CMD_COPY_IMAGE_TO_BUFFER) {
		stub_put64(&fixture_wire, READBACK_IMAGE);
		stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		stub_put64(&fixture_wire, FIXTURE_DST_BUFFER);
	} else {
		stub_put64(&fixture_wire, FIXTURE_DST_BUFFER);
		stub_put64(&fixture_wire, READBACK_IMAGE);
		stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}

	/* One region; sample coordinates and buffer strides are unmodified. */
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, region->bufferOffset);
	stub_put32(&fixture_wire, region->bufferRowLength);
	stub_put32(&fixture_wire, region->bufferImageHeight);
	stub_put32(&fixture_wire, region->imageSubresource.aspectMask);
	stub_put32(&fixture_wire, region->imageSubresource.mipLevel);
	stub_put32(&fixture_wire, region->imageSubresource.baseArrayLayer);
	stub_put32(&fixture_wire, region->imageSubresource.layerCount);
	stub_put32(&fixture_wire, (uint32_t)region->imageOffset.x);
	stub_put32(&fixture_wire, (uint32_t)region->imageOffset.y);
	stub_put32(&fixture_wire, (uint32_t)region->imageOffset.z);
	stub_put32(&fixture_wire, region->imageExtent.width);
	stub_put32(&fixture_wire, region->imageExtent.height);
	stub_put32(&fixture_wire, region->imageExtent.depth);
}

/* Submit a recorded transfer and return its Vulkan status. */
static uint32_t
readback_run(
	const VkBufferImageCopy *region,
	uint32_t opcode)
{
	size_t reply_bytes;
	uint32_t status;

	/* Re-record from an empty operation list; rejected regions cannot reuse a prior copy. */
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	readback_copy(region, opcode);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	stub_rect_calls = 0U;
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	status = stub_get32(stub_reply, 20U);

	/* Succeeded: the fixture received the submission's result. */
	return status;
}

/* Check plane interpretation, buffer packing and unsupported transfers. */
static void
test_readback(void)
{
	VkBufferImageCopy region;
	struct i915_gfx_image *image;
	uint32_t status;

	/* Open the same bound-memory session used by the established transfer tests. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);
	fixture_resources();
	fixture_copy_buffers();
	readback_resources();
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, READBACK_IMAGE);
	assert(image != NULL);

	/* A cropped Y rectangle keeps image offsets and a padded linear buffer pitch. */
	memset(&region, 0, sizeof(region));
	region.bufferOffset = 32U;
	region.bufferRowLength = 72U;
	region.bufferImageHeight = 36U;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageOffset.x = 2;
	region.imageOffset.y = 3;
	region.imageExtent.width = 64U;
	region.imageExtent.height = 31U;
	region.imageExtent.depth = 1U;
	status = readback_run(&region, FIXTURE_CMD_COPY_IMAGE_TO_BUFFER);
	assert(status == VK_SUCCESS);
	assert(stub_rect_calls == 1U);
	assert(stub_rects[0].src.va == FIXTURE_STORAGE_VA + READBACK_IMAGE_OFFSET);
	assert(stub_rects[0].src.width == 66U);
	assert(stub_rects[0].src.height == 34U);
	assert(stub_rects[0].src.format == VK_FORMAT_R8_UNORM);
	assert(stub_rects[0].src.pitch == 128U);
	assert(stub_rects[0].src.tiled == 1U);
	assert(stub_rects[0].src_rect.x == 2);
	assert(stub_rects[0].src_rect.y == 3);
	assert(stub_rects[0].dst.va == FIXTURE_STORAGE_VA + FIXTURE_DST_OFFSET + 32U);
	assert(stub_rects[0].dst.pitch == 72U);
	assert(stub_rects[0].dst.tiled == 0U);

	/* CbCr coordinates count pairs, not luma pixels or individual bytes. */
	region.bufferOffset = 4096U;
	region.bufferRowLength = 36U;
	region.bufferImageHeight = 18U;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	region.imageOffset.x = 1;
	region.imageOffset.y = 1;
	region.imageExtent.width = 32U;
	region.imageExtent.height = 16U;
	status = readback_run(&region, FIXTURE_CMD_COPY_IMAGE_TO_BUFFER);
	assert(status == VK_SUCCESS);
	assert(stub_rect_calls == 1U);
	assert(stub_rects[0].src.va == FIXTURE_STORAGE_VA + READBACK_IMAGE_OFFSET + image->chroma_offset);
	assert(stub_rects[0].src.width == 33U);
	assert(stub_rects[0].src.height == 17U);
	assert(stub_rects[0].src.format == VK_FORMAT_R8G8_UNORM);
	assert(stub_rects[0].dst.format == VK_FORMAT_R8G8_UNORM);
	assert(stub_rects[0].dst.pitch == 72U);
	assert(stub_rects[0].src_rect.x == 1);
	assert(stub_rects[0].src_rect.w == 32U);

	/* Combined aspects cannot select a compatible single plane format. */
	region.imageSubresource.aspectMask |= VK_IMAGE_ASPECT_PLANE_0_BIT;
	status = readback_run(&region, FIXTURE_CMD_COPY_IMAGE_TO_BUFFER);
	assert(status != VK_SUCCESS);
	assert(stub_rect_calls == 0U);
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;

	/* A chroma rectangle measured in luma pixels must not reach the GPU. */
	region.imageExtent.width = 64U;
	status = readback_run(&region, FIXTURE_CMD_COPY_IMAGE_TO_BUFFER);
	assert(status != VK_SUCCESS);
	assert(stub_rect_calls == 0U);
	region.imageExtent.width = 32U;

	/* A wrapped destination offset and an overflowing pitch are refused. */
	region.bufferOffset = UINT64_MAX - 8U;
	status = readback_run(&region, FIXTURE_CMD_COPY_IMAGE_TO_BUFFER);
	assert(status != VK_SUCCESS);
	assert(stub_rect_calls == 0U);
	region.bufferOffset = 4096U;
	region.bufferRowLength = UINT32_MAX;
	status = readback_run(&region, FIXTURE_CMD_COPY_IMAGE_TO_BUFFER);
	assert(status != VK_SUCCESS);
	assert(stub_rect_calls == 0U);
	region.bufferRowLength = 36U;

	/* NV12 uploads remain unsupported; admission exposes readback only. */
	status = readback_run(&region, FIXTURE_CMD_COPY_BUFFER_TO_IMAGE);
	assert(status != VK_SUCCESS);
	assert(stub_rect_calls == 0U);

	/* Tear down every owned resource through the production destructors. */
	stub_session_close();
	assert(stub_live == 0U);
}
