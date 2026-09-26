/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for the resource wire decode.  Encodes the libvulkan streams
 * for vkAllocateMemory/vkFreeMemory, vkCreateBuffer/vkBindBufferMemory/
 * vkDestroyBuffer and vkCreateImage/vkBindImageMemory/vkDestroyImage
 * exactly as userland/base/libvulkan does, drives them through the node's
 * entry (render/vulkan.c) and the router into the resource objects
 * (render/objects.c, memory.c, image.c), and checks the object table and
 * the reply framing: opcode echo, VkResult, output identity, and a reply
 * region selected and positioned by the transport commands.  The format
 * queries report only what the executor implements.
 */

#include "i915-vk-render-stubs.inc"

/* The wire opcodes the fixture sends, as libvulkan numbers them. */
#define FIXTURE_FORMAT_PROPERTIES	4U
#define FIXTURE_IMAGE_FORMAT_PROPERTIES	5U
#define FIXTURE_ALLOCATE_MEMORY		21U
#define FIXTURE_FREE_MEMORY		22U
#define FIXTURE_BIND_BUFFER_MEMORY	28U
#define FIXTURE_BIND_IMAGE_MEMORY	29U
#define FIXTURE_CREATE_BUFFER		50U
#define FIXTURE_DESTROY_BUFFER		51U
#define FIXTURE_CREATE_IMAGE		54U
#define FIXTURE_DESTROY_IMAGE		55U
#define FIXTURE_VERSION_PROBE		137U
#define FIXTURE_SEEK_REPLY		179U

/* The API version the executor reports in the version probe: 1.1. */
#define FIXTURE_API_VERSION		((1U << 22) | (1U << 12))

/* The wire identities the fixture gives its objects. */
#define FIXTURE_DEVICE		0xd0ULL
#define FIXTURE_SOLO		0x123456789aULL
#define FIXTURE_MEMORY		0x100ULL
#define FIXTURE_BUFFER		0x200ULL
#define FIXTURE_IMAGE		0x300ULL
#define FIXTURE_TRANSACTION	0x777ULL

/* The stream every command is built in. */
static struct stub_wire fixture_wire;

static void fixture_allocate_memory(uint64_t identity, uint64_t size, uint32_t reply);
static void fixture_destroy(uint32_t opcode, uint64_t identity, uint32_t reply);
static uint32_t fixture_format_features(uint32_t format, int optimal);
static uint32_t fixture_image_format(uint32_t format, uint32_t type, uint32_t tiling, uint32_t usage, uint32_t *max_levels);
static void test_format_properties(void);
static void test_memory(void);
static void test_buffer_image(void);
static void test_transport(void);

/*
 * Runs the resource decode checks.
 */
int
main(void)
{
	/* Checks the memory commands, the resources bound to memory, then the reply transport. */
	test_memory();
	test_buffer_image();
	test_transport();
	test_format_properties();

	/* Succeeded: every check held. */
	printf("i915 vk resdispatch host test PASS\n");
	return 0;
}

/* Appends vkAllocateMemory of memory type 0, asking for a reply when `reply` is nonzero. */
static void
fixture_allocate_memory(
	uint64_t identity,
	uint64_t size,
	uint32_t reply)
{
	/* The header and the device. */
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_MEMORY);
	stub_put32(&fixture_wire, reply);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);

	/* VkMemoryAllocateInfo behind its presence marker: sType 5, no chain, the size, type 0. */
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 5U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, size);
	stub_put32(&fixture_wire, 0U);

	/* No allocator, then the identity behind its presence marker. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends a generic destroy or free: [opcode][reply][device][identity][pAllocator]. */
static void
fixture_destroy(
	uint32_t opcode,
	uint64_t identity,
	uint32_t reply)
{
	/* The command has no reply body. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, reply);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, identity);
	stub_put64(&fixture_wire, 0U);
}

/*
 * vkAllocateMemory publishes the allocation and answers with its identity;
 * vkFreeMemory, a void command, answers with the echoed opcode alone.
 */
static void
test_memory(void)
{
	size_t reply_bytes;

	/* Opens the fixture session. */
	stub_session_open(NULL);

	/* [21][VK_SUCCESS][present][identity]. */
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_SOLO, 65536U, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_ALLOCATE_MEMORY);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 8U) == 1U);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_SOLO);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_SOLO) != NULL);

	/* [22] and nothing else; the identity leaves the table. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_SOLO, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_FREE_MEMORY);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_SOLO) == NULL);

	/* A void command that asks for no reply writes none, and still runs. */
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_SOLO, 4096U, 1U);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_SOLO, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_SOLO) == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A buffer and an image are created, bound into memory and destroyed
 * through the wire, each answer framed as libvulkan reads it.
 */
static void
test_buffer_image(void)
{
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_image *image;
	size_t reply_bytes;
	int error;

	/* Opens the fixture session with memory to bind into. */
	stub_session_open(NULL);
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_MEMORY, 1048576U, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);

	/* vkCreateBuffer of 4096 vertex-buffer bytes, no queue families: [50][VK_SUCCESS][present][identity]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 12U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 4096U);
	stub_put32(&fixture_wire, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_CREATE_BUFFER);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_BUFFER);
	buffer = drv_i915_object_lookup(stub_session, I915_VK_OBJ_BUFFER, FIXTURE_BUFFER);
	assert(buffer != NULL);

	/* vkBindBufferMemory at offset 0: [28][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_BIND_BUFFER_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_BIND_BUFFER_MEMORY);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(buffer->memory != NULL);

	/* vkCreateImage of a 320x240 R8G8B8A8_UNORM image: [54][VK_SUCCESS][present][identity]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 14U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_R8G8B8A8_UNORM);
	stub_put32(&fixture_wire, 320U);
	stub_put32(&fixture_wire, 240U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_SAMPLE_COUNT_1_BIT);
	stub_put32(&fixture_wire, VK_IMAGE_TILING_LINEAR);
	stub_put32(&fixture_wire, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_UNDEFINED);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_CREATE_IMAGE);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_IMAGE);
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE);
	assert(image != NULL);

	/* vkBindImageMemory behind the buffer: [29][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_BIND_IMAGE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, 4096U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(image->memory == buffer->memory);
	assert(image->offset == 4096U);

	/* A bind to memory that does not exist fails in the reply, not in the stream. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_BIND_IMAGE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put64(&fixture_wire, FIXTURE_MEMORY + 1U);
	stub_put64(&fixture_wire, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);

	/* A create cut short inside its record fails the stream and publishes no reply length. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 12U);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == EINVAL);
	assert(reply_bytes == STUB_REPLY_BYTES);

	/* Destroys the image and the buffer, then frees the memory; each is a bare echo and leaves the table. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE, 1U);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER, 1U);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 12U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_DESTROY_IMAGE);
	assert(stub_get32(stub_reply, 4U) == FIXTURE_DESTROY_BUFFER);
	assert(stub_get32(stub_reply, 8U) == FIXTURE_FREE_MEMORY);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE) == NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_BUFFER, FIXTURE_BUFFER) == NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_MEMORY) == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * One transaction as libvulkan sends it: the reply selector names a region
 * of the reply blob, a command answers at its head, a seek moves to the
 * tail, and the version probe writes the completion trailer there.
 */
static void
test_transport(void)
{
	struct i915_gfx_memory *memory;
	size_t reply_bytes;
	size_t capacity;
	size_t seek;
	uint8_t *region;
	int error;

	/* Opens the fixture session. */
	stub_session_open(NULL);

	/* The selector names slot 3 from offset 256; the region runs to the end of the blob. */
	stub_wire_reset(&fixture_wire);
	stub_put32(&fixture_wire, STUB_SET_REPLY_STREAM);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, STUB_REPLY_SLOT);
	stub_put64(&fixture_wire, 256U);
	stub_put64(&fixture_wire, STUB_REPLY_BYTES - 256U);
	region = drv_i915_render_transport_reply(&stub_gpu, fixture_wire.bytes, (uint32_t)fixture_wire.size, &capacity);
	assert(region == stub_reply + 256);
	assert(capacity == STUB_REPLY_BYTES - 256U);

	/* select, allocate, seek to the last 20 bytes, then the version probe. */
	seek = capacity - 20U;
	fixture_allocate_memory(FIXTURE_TRANSACTION, 4096U, 1U);
	stub_put32(&fixture_wire, FIXTURE_SEEK_REPLY);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, seek);
	stub_put32(&fixture_wire, FIXTURE_VERSION_PROBE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);

	/* Executes the transaction into the region the node resolved. */
	memset(stub_reply, 0, sizeof(stub_reply));
	reply_bytes = capacity;
	error = drv_i915_render_execute(stub_session, fixture_wire.bytes, fixture_wire.size, region, &reply_bytes);
	assert(error == 0);
	assert(reply_bytes == capacity);

	/* The vkAllocateMemory reply sits at the head of the region. */
	assert(stub_get32(region, 0U) == FIXTURE_ALLOCATE_MEMORY);
	assert(stub_get32(region, 4U) == VK_SUCCESS);
	assert(stub_get64(region, 8U) == 1U);
	assert(stub_get64(region, 16U) == FIXTURE_TRANSACTION);

	/* The completion trailer sits at the fixed tail position. */
	assert(stub_get32(region, seek) == FIXTURE_VERSION_PROBE);
	assert(stub_get32(region, seek + 4U) == VK_SUCCESS);
	assert(stub_get64(region, seek + 8U) == 1U);
	assert(stub_get32(region, seek + 16U) == FIXTURE_API_VERSION);

	/* Nothing was written in front of the region. */
	assert(stub_get32(stub_reply, 252U) == 0U);

	/* The memory the transaction created is freed like any other. */
	memory = drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_TRANSACTION);
	assert(memory != NULL);
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_TRANSACTION, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 0U);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/* Queries vkGetPhysicalDeviceFormatProperties and reports the optimal or the linear tiling's features. */
static uint32_t
fixture_format_features(
	uint32_t format,
	int optimal)
{
	size_t reply_bytes;

	/* [4][reply][physical][format][present]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_FORMAT_PROPERTIES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, format);
	stub_put64(&fixture_wire, 1U);

	/* [4][present][linear][optimal][buffer]. */
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_FORMAT_PROPERTIES);
	assert(stub_get64(stub_reply, 4U) == 1U);
	assert(stub_get32(stub_reply, 20U) == 0U);

	/* Reports the optimal tiling's features. */
	if (optimal)
		return stub_get32(stub_reply, 16U);

	/* Reports the linear tiling's features. */
	return stub_get32(stub_reply, 12U);
}

/* Queries vkGetPhysicalDeviceImageFormatProperties; reports the VkResult and, on success, the level limit. */
static uint32_t
fixture_image_format(
	uint32_t format,
	uint32_t type,
	uint32_t tiling,
	uint32_t usage,
	uint32_t *max_levels)
{
	size_t reply_bytes;

	/* [5][reply][physical][format][type][tiling][usage][flags][present]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_IMAGE_FORMAT_PROPERTIES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, format);
	stub_put32(&fixture_wire, type);
	stub_put32(&fixture_wire, tiling);
	stub_put32(&fixture_wire, usage);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);

	/* [5][result][present][extent][levels][layers][samples][size]. */
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 48U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_IMAGE_FORMAT_PROPERTIES);
	assert(stub_get64(stub_reply, 8U) == 1U);
	*max_levels = stub_get32(stub_reply, 28U);

	/* Succeeded: the VkResult of the query. */
	return stub_get32(stub_reply, 4U);
}

/*
 * The format queries claim what the executor implements: RGBA8 and BGRA8
 * are sampled (linearly), rendered to, copied and blitted in both tilings
 * with a full mip chain; D32 is an optimal-tiled depth attachment of one
 * level and nothing else.  A usage the tiling's features do not cover, a
 * storage image and a 3D image are not supported.
 */
static void
test_format_properties(void)
{
	uint32_t colour_features;
	uint32_t features;
	uint32_t result;
	uint32_t levels;

	/* Opens the fixture session. */
	stub_session_open(NULL);

	/* RGBA8 and BGRA8 have every colour feature in both tilings. */
	colour_features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
	    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	    VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
	    VK_FORMAT_FEATURE_BLIT_SRC_BIT |
	    VK_FORMAT_FEATURE_BLIT_DST_BIT;
	features = fixture_format_features(VK_FORMAT_R8G8B8A8_UNORM, 1);
	assert(features == colour_features);
	features = fixture_format_features(VK_FORMAT_R8G8B8A8_UNORM, 0);
	assert(features == colour_features);
	features = fixture_format_features(VK_FORMAT_B8G8R8A8_UNORM, 1);
	assert(features == colour_features);

	/* D32 is an optimal-tiled depth attachment only; an unknown format has nothing. */
	features = fixture_format_features(VK_FORMAT_D32_SFLOAT, 1);
	assert(features == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
	features = fixture_format_features(VK_FORMAT_D32_SFLOAT, 0);
	assert(features == 0U);
	features = fixture_format_features(VK_FORMAT_R16G16B16A16_SFLOAT, 1);
	assert(features == 0U);

	/* The display's optimal colour and transfer image, and its linear sampled copy, are supported with 15 levels. */
	levels = 0U;
	result = fixture_image_format(VK_FORMAT_B8G8R8A8_UNORM,
				      VK_IMAGE_TYPE_2D,
				      VK_IMAGE_TILING_OPTIMAL,
				      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				      &levels);
	assert(result == VK_SUCCESS);
	assert(levels == 15U);
	result = fixture_image_format(VK_FORMAT_R8G8B8A8_UNORM,
				      VK_IMAGE_TYPE_2D,
				      VK_IMAGE_TILING_LINEAR,
				      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				      &levels);
	assert(result == VK_SUCCESS);

	/* A depth attachment has one level. */
	result = fixture_image_format(VK_FORMAT_D32_SFLOAT,
				      VK_IMAGE_TYPE_2D,
				      VK_IMAGE_TILING_OPTIMAL,
				      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				      &levels);
	assert(result == VK_SUCCESS);
	assert(levels == 1U);

	/* A sampled depth image, a linear depth image and a colour storage image are not supported. */
	result = fixture_image_format(VK_FORMAT_D32_SFLOAT,
				      VK_IMAGE_TYPE_2D,
				      VK_IMAGE_TILING_OPTIMAL,
				      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				      &levels);
	assert(result == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
	result = fixture_image_format(VK_FORMAT_D32_SFLOAT,
				      VK_IMAGE_TYPE_2D,
				      VK_IMAGE_TILING_LINEAR,
				      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				      &levels);
	assert(result == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
	result = fixture_image_format(VK_FORMAT_R8G8B8A8_UNORM,
				      VK_IMAGE_TYPE_2D,
				      VK_IMAGE_TILING_OPTIMAL,
				      VK_IMAGE_USAGE_STORAGE_BIT,
				      &levels);
	assert(result == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);

	/* A 3D image is not supported. */
	result = fixture_image_format(VK_FORMAT_R8G8B8A8_UNORM,
				      VK_IMAGE_TYPE_3D,
				      VK_IMAGE_TILING_OPTIMAL,
				      VK_IMAGE_USAGE_SAMPLED_BIT,
				      &levels);
	assert(result == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);

	/* Closes the fixture session. */
	stub_session_close();
}
