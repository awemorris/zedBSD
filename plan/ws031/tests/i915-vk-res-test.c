/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for the executor's resources: device memory and its blob
 * storage, buffers, images and their layout, the surface state an image
 * is drawn through, and descriptors (render/memory.c, image.c,
 * descriptor.c, state.c).
 *
 * The objects are created through the wire, as libvulkan creates them; the
 * checks then look at the objects the executor published and at the
 * functions the draw path reads them through.
 */

#include "i915-vk-render-stubs.inc"

#include "../../../src/drivers/gpu/i915/render/heap.h"
#include "../../../src/drivers/gpu/i915/render/state.h"

/* The wire opcodes the fixture sends, as libvulkan numbers them. */
#define FIXTURE_ALLOCATE_MEMORY			21U
#define FIXTURE_FREE_MEMORY			22U
#define FIXTURE_BIND_BUFFER_MEMORY		28U
#define FIXTURE_BIND_IMAGE_MEMORY		29U
#define FIXTURE_GET_IMAGE_MEMORY_REQUIREMENTS	31U
#define FIXTURE_CREATE_BUFFER			50U
#define FIXTURE_DESTROY_BUFFER			51U
#define FIXTURE_CREATE_IMAGE			54U
#define FIXTURE_DESTROY_IMAGE			55U
#define FIXTURE_GET_SUBRESOURCE_LAYOUT		56U
#define FIXTURE_CREATE_IMAGE_VIEW		57U
#define FIXTURE_DESTROY_IMAGE_VIEW		58U
#define FIXTURE_CREATE_SAMPLER			70U
#define FIXTURE_DESTROY_SAMPLER			71U
#define FIXTURE_CREATE_DSL			72U
#define FIXTURE_DESTROY_DSL			73U
#define FIXTURE_CREATE_DESCRIPTOR_POOL		74U
#define FIXTURE_DESTROY_DESCRIPTOR_POOL		75U
#define FIXTURE_ALLOCATE_DESCRIPTOR_SETS	77U
#define FIXTURE_FREE_DESCRIPTOR_SETS		78U
#define FIXTURE_UPDATE_DESCRIPTOR_SETS		79U

/* The wire identities the fixture gives its objects. */
#define FIXTURE_DEVICE		0xd0ULL
#define FIXTURE_MEMORY		0x100ULL
#define FIXTURE_BUFFER		0x200ULL
#define FIXTURE_IMAGE		0x300ULL
#define FIXTURE_DEPTH		0x301ULL
#define FIXTURE_MIPMAPPED	0x302ULL
#define FIXTURE_VIEW		0x400ULL
#define FIXTURE_SAMPLER		0x500ULL
#define FIXTURE_DSL		0x600ULL
#define FIXTURE_WIDE_DSL	0x601ULL
#define FIXTURE_POOL		0x700ULL
#define FIXTURE_SET		0x800ULL

/* The storage blob: its size and the GPU address it is bound at. */
#define FIXTURE_STORAGE_BYTES	(512U * 1024U)
#define FIXTURE_STORAGE_VA	0x100000000ULL

/* The image the fixture draws through: 320x240 R8G8B8A8_UNORM, 1280 bytes to a row. */
#define FIXTURE_WIDTH		320U
#define FIXTURE_HEIGHT		240U
#define FIXTURE_PITCH		1280U

/*
 * The storage libvulkan exports for the fixture's allocation, and the
 * session object that stands for that blob.
 */
static uint8_t fixture_storage[FIXTURE_STORAGE_BYTES] __attribute__((aligned(4096)));
static struct i915_gem_object fixture_storage_object;

/* The state object a draw's state is written into, as the draw path writes it on the GPU's page. */
static uint8_t fixture_state_page[I915_GFX_STATE_BYTES] __attribute__((aligned(4096)));

/* The stream every command is built in. */
static struct stub_wire fixture_wire;

static void fixture_allocate_memory(uint64_t identity, uint64_t size, uint32_t type);
static void fixture_destroy(uint32_t opcode, uint64_t identity);
static void fixture_create_image(uint64_t identity, uint32_t format, uint32_t width, uint32_t height, uint32_t levels);
static void fixture_bind(uint32_t opcode, uint64_t resource, uint64_t offset);
static void fixture_create_view(uint64_t identity, uint64_t image, uint32_t base_level, uint32_t level_count);
static void fixture_create_sampler(uint64_t identity, uint32_t mipmap_mode, uint32_t lod_bias, uint32_t min_lod, uint32_t max_lod);
static void test_memory_storage(void);
static void test_buffer_image(void);
static void test_mip_images(void);
static void test_descriptors(void);

/*
 * Runs the resource checks.
 */
int
main(void)
{
	/* Checks memory, then the resources bound to it, mipmapped images and samplers, then descriptors. */
	test_memory_storage();
	test_buffer_image();
	test_mip_images();
	test_descriptors();

	/* Succeeded: every check held. */
	printf("i915 vk res host test PASS\n");
	return 0;
}

/* Appends vkAllocateMemory for an allocation of `size` bytes of memory type `type`. */
static void
fixture_allocate_memory(
	uint64_t identity,
	uint64_t size,
	uint32_t type)
{
	/* [21][reply][device][present][sType 5][no chain][size][type][pAllocator][present][identity]. */
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 5U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, size);
	stub_put32(&fixture_wire, type);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends a generic destroy or free: [opcode][reply][device][identity][pAllocator]. */
static void
fixture_destroy(
	uint32_t opcode,
	uint64_t identity)
{
	/* The command has no reply body; the reply is its echoed opcode. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, identity);
	stub_put64(&fixture_wire, 0U);
}

/* Appends vkCreateImage for a 2D image with `levels` mip levels. */
static void
fixture_create_image(
	uint64_t identity,
	uint32_t format,
	uint32_t width,
	uint32_t height,
	uint32_t levels)
{
	/* The header, the device and the create info's presence marker. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);

	/* VkImageCreateInfo: sType 14, no chain, flags, 2D, the format and the extent. */
	stub_put32(&fixture_wire, 14U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_TYPE_2D);
	stub_put32(&fixture_wire, format);
	stub_put32(&fixture_wire, width);
	stub_put32(&fixture_wire, height);
	stub_put32(&fixture_wire, 1U);

	/* The levels, one layer, one sample, optimal tiling, colour attachment and sampled usage. */
	stub_put32(&fixture_wire, levels);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_SAMPLE_COUNT_1_BIT);
	stub_put32(&fixture_wire, VK_IMAGE_TILING_OPTIMAL);
	stub_put32(&fixture_wire, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	/* Exclusive sharing with no queue families, and an undefined initial layout. */
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_UNDEFINED);

	/* The tail: no allocator, the identity behind its presence marker. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends vkBindBufferMemory or vkBindImageMemory of a resource to the fixture's memory. */
static void
fixture_bind(
	uint32_t opcode,
	uint64_t resource,
	uint64_t offset)
{
	/* [opcode][reply][device][resource][memory][offset]. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, resource);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, offset);
}

/* Appends vkCreateImageView of a 2D RGBA8 view of the levels [base_level, base_level + level_count). */
static void
fixture_create_view(
	uint64_t identity,
	uint64_t image,
	uint32_t base_level,
	uint32_t level_count)
{
	unsigned index;

	/* The header, the device and the create info's presence marker; sType 15, no chain, no flags. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE_VIEW);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 15U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);

	/* The image, the view type, the format and identity swizzles. */
	stub_put64(&fixture_wire, image);
	stub_put32(&fixture_wire, VK_IMAGE_VIEW_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_R8G8B8A8_UNORM);
	for (index = 0U; index < 4U; index++)
		stub_put32(&fixture_wire, 0U);

	/* The subresource range: colour, the levels, one layer. */
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, base_level);
	stub_put32(&fixture_wire, level_count);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);

	/* The tail: no allocator, the identity behind its presence marker. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends vkCreateSampler of a linear sampler, repeating along u and v, with a mipmap mode and float-bit LODs. */
static void
fixture_create_sampler(
	uint64_t identity,
	uint32_t mipmap_mode,
	uint32_t lod_bias,
	uint32_t min_lod,
	uint32_t max_lod)
{
	/* The header, the device and the create info's presence marker; sType 31, no chain, no flags. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_SAMPLER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 31U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);

	/* Linear filters, the mipmap mode, repeat along u, v and w. */
	stub_put32(&fixture_wire, VK_FILTER_LINEAR);
	stub_put32(&fixture_wire, VK_FILTER_LINEAR);
	stub_put32(&fixture_wire, mipmap_mode);
	stub_put32(&fixture_wire, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	stub_put32(&fixture_wire, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	stub_put32(&fixture_wire, VK_SAMPLER_ADDRESS_MODE_REPEAT);

	/* The bias; no anisotropy (ratio 1.0); no comparison; the LOD range; border and coordinates zero. */
	stub_put32(&fixture_wire, lod_bias);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_COMPARE_OP_ALWAYS);
	stub_put32(&fixture_wire, min_lod);
	stub_put32(&fixture_wire, max_lod);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);

	/* The tail: no allocator, the identity behind its presence marker. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/*
 * An allocation has no storage until its blob arrives; the blob is found
 * by the allocation's identity, and the CPU and GPU views follow it.
 */
static void
test_memory_storage(void)
{
	struct i915_gem_object small;
	struct i915_gfx_memory *memory;
	size_t reply_bytes;
	uint8_t *view;
	uint64_t va;
	int error;

	/* Opens a session with the storage blob among its objects. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);

	/* A blob that names no allocation is refused as unknown. */
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == ENOENT);

	/* Allocates 64 KiB of memory type 0: [21][VK_SUCCESS][present][identity]. */
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_MEMORY, 65536U, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	memory = drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_MEMORY);
	assert(memory != NULL);
	assert(memory->size == 65536U);

	/* Before its blob arrives the allocation has neither a CPU nor a GPU view. */
	view = drv_i915_gfx_memory_cpu(memory, 0U, 4U);
	assert(view == NULL);
	va = drv_i915_gfx_memory_va(memory, 0U);
	assert(va == 0U);

	/* A blob smaller than the allocation cannot be its storage. */
	memset(&small, 0, sizeof(small));
	small.bytes = 4096U;
	small.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &small);
	assert(error == EINVAL);

	/* The blob libvulkan exports for the allocation becomes its storage, once. */
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == 0);
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == EINVAL);

	/* The views are the blob's host pointer and GPU address plus the offset. */
	view = drv_i915_gfx_memory_cpu(memory, 4096U, 16U);
	assert(view == fixture_storage + 4096);
	va = drv_i915_gfx_memory_va(memory, 4096U);
	assert(va == FIXTURE_STORAGE_VA + 4096U);

	/* A range that runs past the blob has no CPU view. */
	view = drv_i915_gfx_memory_cpu(memory, FIXTURE_STORAGE_BYTES - 8U, 16U);
	assert(view == NULL);

	/* A blob that goes away takes the storage with it. */
	drv_i915_render_blob_detach(stub_vk, &fixture_storage_object);
	view = drv_i915_gfx_memory_cpu(memory, 0U, 4U);
	assert(view == NULL);

	/* Memory type 1 and an empty allocation are refused as initialization failures. */
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_MEMORY + 1U, 65536U, 1U);
	fixture_allocate_memory(FIXTURE_MEMORY + 2U, 0U, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(stub_get32(stub_reply, 12U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	memory = drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_MEMORY + 1U);
	assert(memory == NULL);

	/* vkFreeMemory releases the allocation; nothing the test made stays allocated. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_MEMORY) == NULL);
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A buffer and an image bind into an allocation they fit; the image's
 * layout, requirements and surface state are what the draw path reads.
 */
static void
test_buffer_image(void)
{
	struct i915_gfx_surface surface;
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_image *image;
	struct i915_gfx_view *view;
	uint32_t rss[16];
	size_t reply_bytes;
	int error;

	/* Opens a session and gives an allocation its storage. */
	stub_session_open(&fixture_storage_object);
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_MEMORY, FIXTURE_STORAGE_BYTES, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == 0);

	/* vkCreateBuffer of 4096 vertex-buffer bytes, then its bind at offset 0. */
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
	fixture_bind(FIXTURE_BIND_BUFFER_MEMORY, FIXTURE_BUFFER, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 8U);
	assert(stub_get32(stub_reply, 28U) == VK_SUCCESS);
	buffer = drv_i915_object_lookup(stub_session, I915_VK_OBJ_BUFFER, FIXTURE_BUFFER);
	assert(buffer != NULL);
	assert(buffer->size == 4096U);
	assert(buffer->usage == VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	assert(buffer->memory == drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_MEMORY));

	/* A bind that would run past the end of the allocation fails and leaves the buffer as it was. */
	stub_wire_begin(&fixture_wire);
	fixture_bind(FIXTURE_BIND_BUFFER_MEMORY, FIXTURE_BUFFER, FIXTURE_STORAGE_BYTES - 16U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(buffer->offset == 0U);

	/* A 320x240 R8G8B8A8_UNORM image is linear rows of 1280 bytes. */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_IMAGE, VK_FORMAT_R8G8B8A8_UNORM, FIXTURE_WIDTH, FIXTURE_HEIGHT, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE);
	assert(image != NULL);
	assert(image->pitch == FIXTURE_PITCH);
	assert(image->bytes == (uint64_t)FIXTURE_PITCH * FIXTURE_HEIGHT);

	/* Its requirements are whole pages of memory type 0: [31][present][size][alignment][type bits]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_IMAGE_MEMORY_REQUIREMENTS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put64(&fixture_wire, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 32U);
	assert(stub_get64(stub_reply, 12U) == 307200U);
	assert(stub_get64(stub_reply, 20U) == 4096U);
	assert(stub_get32(stub_reply, 28U) == 1U);

	/* Its one subresource: [56][present][offset][size][row pitch][array pitch][depth pitch]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_SUBRESOURCE_LAYOUT);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 52U);
	assert(stub_get64(stub_reply, 12U) == 0U);
	assert(stub_get64(stub_reply, 20U) == 307200U);
	assert(stub_get64(stub_reply, 28U) == FIXTURE_PITCH);

	/* A depth image is laid out in whole Y tiles: 100x50 takes 512-byte rows and 64 rows. */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_DEPTH, VK_FORMAT_D32_SFLOAT, 100U, 50U, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_DEPTH);
	assert(image != NULL);
	assert(image->pitch == 512U);
	assert(image->bytes == 512U * 64U);

	/* A depth image with two mip levels is refused by name as a missing feature. */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_MIPMAPPED, VK_FORMAT_D32_SFLOAT, 64U, 64U, 2U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
	assert(strstr(stub_log, "XXX vkCreateImage refused") != NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_MIPMAPPED) == NULL);

	/* A colour image with more levels than halve 64 down to one texel (7) is refused the same way. */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_MIPMAPPED, VK_FORMAT_R8G8B8A8_UNORM, 64U, 64U, 8U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_MIPMAPPED) == NULL);

	/* Binds the colour image behind the buffer and creates the whole-image view. */
	stub_wire_begin(&fixture_wire);
	fixture_bind(FIXTURE_BIND_IMAGE_MEMORY, FIXTURE_IMAGE, 65536U);
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE_VIEW);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 15U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put32(&fixture_wire, VK_IMAGE_VIEW_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_R8G8B8A8_UNORM);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_VIEW);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U + 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE);
	view = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE_VIEW, FIXTURE_VIEW);
	assert(view != NULL);
	assert(view->image == image);
	assert(image->offset == 65536U);

	/* The surface a draw writes the image through, described as the draw path describes it. */
	memset(&surface, 0, sizeof(surface));
	surface.va = drv_i915_gfx_memory_va(image->memory, image->offset);
	surface.width = image->width;
	surface.height = image->height;
	surface.pitch = image->pitch;
	surface.format = image->format;
	error = drv_i915_gfx_surface_write(rss, &surface, 0U);
	assert(error == 0);

	/* Dword 0: a 2D, linear, R8G8B8A8_UNORM surface. */
	assert(((rss[0] >> 29) & 0x7U) == 1U);
	assert(((rss[0] >> 18) & 0x3ffU) == 0x0c7U);
	assert(((rss[0] >> 12) & 0x3U) == 0U);

	/* Dword 2: the extent minus one; dword 3: the pitch minus one. */
	assert((rss[2] & 0x3fffU) == FIXTURE_WIDTH - 1U);
	assert(((rss[2] >> 16) & 0x3fffU) == FIXTURE_HEIGHT - 1U);
	assert((rss[3] & 0x3ffffU) == FIXTURE_PITCH - 1U);

	/* Dwords 8 and 9: the image's GPU address in the storage blob. */
	assert(rss[8] == (uint32_t)(FIXTURE_STORAGE_VA + 65536U));
	assert(rss[9] == (uint32_t)((FIXTURE_STORAGE_VA + 65536U) >> 32));

	/* A surface with no storage has no surface state. */
	surface.va = 0U;
	error = drv_i915_gfx_surface_write(rss, &surface, 0U);
	assert(error == EINVAL);

	/* Every object is destroyed through the wire and nothing stays allocated. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_IMAGE_VIEW, FIXTURE_VIEW);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_DEPTH);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 5U * 4U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE) == NULL);
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A mipmapped image takes the 2D mip layout with one pitch for every level
 * (level 1 below level 0, level 2 to its right, the rest below level 2);
 * its memory covers the whole chain, each level can be described on its
 * own, a view keeps its range of levels, a sampler its mipmap mode, bias
 * and LOD range, and the surface and sampler state a draw writes carry them.
 */
static void
test_mip_images(void)
{
	static const uint32_t code[4] = { 0U, 0U, 0U, 0U };
	struct i915_gfx_image *image;
	struct i915_gfx_image *odd;
	struct i915_gfx_image *target;
	struct i915_gfx_view *view;
	struct i915_gfx_sampler *sampler;
	struct i915_gfx_surface surface;
	struct i915_gfx_pipeline pipeline;
	struct i915_gfx_dset dset;
	struct i915_gfx_draw_state state;
	struct i915_gfx_kernels kernels;
	uint32_t words[4];
	const uint32_t *rss;
	size_t reply_bytes;
	int error;

	/* Opens a session and gives an allocation its storage. */
	stub_session_open(&fixture_storage_object);
	stub_wire_begin(&fixture_wire);
	fixture_allocate_memory(FIXTURE_MEMORY, FIXTURE_STORAGE_BYTES, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == 0);

	/*
	 * A 64x64 image of all 7 levels: 64 wide (level 0 above levels 1 and 2,
	 * 32 + 16), 100 rows (64, then 16 + 8 + 4 + 4 + 4 in the right column,
	 * the levels of 2 and 1 texels rounded up to 4): 256-byte rows.
	 */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_MIPMAPPED, VK_FORMAT_R8G8B8A8_UNORM, 64U, 64U, 7U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_MIPMAPPED);
	assert(image != NULL);
	assert(image->levels == 7U);
	assert(image->pitch == 256U);
	assert(image->bytes == 256U * 100U);

	/* Its requirements cover the whole chain in whole pages. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_IMAGE_MEMORY_REQUIREMENTS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_MIPMAPPED);
	stub_put64(&fixture_wire, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 32U);
	assert(stub_get64(stub_reply, 12U) == 28672U);

	/* Level 3 (8x8) starts at row 80, column 32, and ends with its last texel. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_SUBRESOURCE_LAYOUT);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_MIPMAPPED);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, 3U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 52U);
	assert(stub_get64(stub_reply, 12U) == 80U * 256U + 32U * 4U);
	assert(stub_get64(stub_reply, 20U) == 7U * 256U + 8U * 4U);
	assert(stub_get64(stub_reply, 28U) == 256U);

	/* A 5x3 image of its 3 levels: every level rounds up to 4x4 texels, so 8 by 8 in all. */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_MIPMAPPED + 1U, VK_FORMAT_R8G8B8A8_UNORM, 5U, 3U, 3U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	odd = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_MIPMAPPED + 1U);
	assert(odd != NULL);
	assert(odd->pitch == 32U);
	assert(odd->bytes == 32U * 8U);

	/* Binds the chain at the start of the storage and a 16x16 one-level target after it. */
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_IMAGE, VK_FORMAT_R8G8B8A8_UNORM, 16U, 16U, 1U);
	fixture_bind(FIXTURE_BIND_IMAGE_MEMORY, FIXTURE_MIPMAPPED, 0U);
	fixture_bind(FIXTURE_BIND_IMAGE_MEMORY, FIXTURE_IMAGE, 65536U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 8U + 8U);
	target = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE);
	assert(target != NULL);

	/* Level 2 (16x16) is described on its own: row 64, column 32, the image's pitch. */
	error = drv_i915_gfx_image_level(image, 2U, &surface);
	assert(error == 0);
	assert(surface.va == FIXTURE_STORAGE_VA + 64U * 256U + 32U * 4U);
	assert(surface.width == 16U);
	assert(surface.height == 16U);
	assert(surface.pitch == 256U);

	/* Level 6 is one texel at row 96, column 32; there is no level 7. */
	error = drv_i915_gfx_image_level(image, 6U, &surface);
	assert(error == 0);
	assert(surface.va == FIXTURE_STORAGE_VA + 96U * 256U + 32U * 4U);
	assert(surface.width == 1U);
	assert(surface.height == 1U);
	error = drv_i915_gfx_image_level(image, 7U, &surface);
	assert(error == EINVAL);

	/* A view of levels 1 on (the remaining levels) keeps 6 levels from level 1. */
	stub_wire_begin(&fixture_wire);
	fixture_create_view(FIXTURE_VIEW, FIXTURE_MIPMAPPED, 1U, VK_REMAINING_MIP_LEVELS);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	view = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE_VIEW, FIXTURE_VIEW);
	assert(view != NULL);
	assert(view->base_level == 1U);
	assert(view->level_count == 6U);

	/* A view that starts past the last level, or that has no level, is refused. */
	stub_wire_begin(&fixture_wire);
	fixture_create_view(FIXTURE_VIEW + 1U, FIXTURE_MIPMAPPED, 7U, 1U);
	fixture_create_view(FIXTURE_VIEW + 2U, FIXTURE_MIPMAPPED, 0U, 0U);
	fixture_create_view(FIXTURE_VIEW + 3U, FIXTURE_MIPMAPPED, 5U, 3U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 3U * 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(stub_get32(stub_reply, 12U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(stub_get32(stub_reply, 20U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(strstr(stub_log, "vkCreateImageView refused") != NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE_VIEW, FIXTURE_VIEW + 1U) == NULL);

	/* A trilinear sampler with bias 1.5 and LOD range [0.25, 16] keeps them as float bits. */
	stub_wire_begin(&fixture_wire);
	fixture_create_sampler(FIXTURE_SAMPLER, VK_SAMPLER_MIPMAP_MODE_LINEAR, 0x3fc00000U, 0x3e800000U, 0x41800000U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	sampler = drv_i915_object_lookup(stub_session, I915_VK_OBJ_SAMPLER, FIXTURE_SAMPLER);
	assert(sampler != NULL);
	assert(sampler->mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
	assert(sampler->lod_bias == 0x3fc00000U);
	assert(sampler->min_lod == 0x3e800000U);
	assert(sampler->max_lod == 0x41800000U);

	/*
	 * Its SAMPLER_STATE: OpenGL pre-clamp, MIPFILTER_LINEAR, linear filters
	 * and bias 384/256; Max LOD clamped to 14 (3584) and Min LOD 64; address
	 * rounding and WRAP along u and v.
	 */
	drv_i915_gfx_sampler_write(words, sampler);
	assert(words[0] == ((2U << 27) | (3U << 20) | (1U << 17) | (1U << 14) | (384U << 1)));
	assert(words[1] == ((3584U << 8) | (64U << 20)));
	assert(words[3] == (0x0007e000U | 2U));

	/* A nearest-level sampler with bias -2 and LOD range [0, 0.5]: MIPFILTER_NEAREST, bias 0x1e00. */
	sampler->mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler->lod_bias = 0xc0000000U;
	sampler->min_lod = 0U;
	sampler->max_lod = 0x3f000000U;
	drv_i915_gfx_sampler_write(words, sampler);
	assert(words[0] == ((2U << 27) | (1U << 20) | (1U << 17) | (1U << 14) | (0x1e00U << 1)));
	assert(words[1] == (128U << 8));

	/* Describes a draw that samples the view through the sampler into the target. */
	memset(&pipeline, 0, sizeof(pipeline));
	memset(&dset, 0, sizeof(dset));
	memset(&state, 0, sizeof(state));
	memset(&kernels, 0, sizeof(kernels));
	dset.slots[0].view = view;
	dset.slots[0].sampler = sampler;
	state.pipeline = &pipeline;
	state.dset[0] = &dset;
	kernels.vs_code = code;
	kernels.vs_bytes = sizeof(code);
	kernels.ps_code = code;
	kernels.ps_bytes = sizeof(code);
	kernels.ps_samplers = 1U;
	error = drv_i915_gfx_write_state(fixture_state_page, &state, &kernels, target, 0U);
	assert(error == 0);

	/*
	 * The texture's surface state: the level-0 extent and the shared pitch
	 * at the chain's start, QPitch 100 / 4, MIP count 5 from Surface Min
	 * LOD 1, mip tail start 7.
	 */
	rss = (const uint32_t *)(const void *)(fixture_state_page + I915_GFX_SURFACE_HEAP + I915_GFX_RSS_TEXTURE);
	assert((rss[1] & 0x7fffU) == 25U);
	assert(rss[2] == (63U | (63U << 16)));
	assert((rss[3] & 0x3ffffU) == 255U);
	assert(rss[5] == (5U | (1U << 4) | (7U << 8)));
	assert(rss[8] == (uint32_t)FIXTURE_STORAGE_VA);

	/* The target's surface state is its one level: MIP count 0, mip tail start 1, QPitch 16 / 4. */
	rss = (const uint32_t *)(const void *)(fixture_state_page + I915_GFX_SURFACE_HEAP + I915_GFX_RSS_TARGET);
	assert(rss[5] == 0x00000100U);
	assert((rss[1] & 0x7fffU) == 4U);

	/* A view with no level cannot be sampled. */
	view->level_count = 0U;
	error = drv_i915_gfx_write_state(fixture_state_page, &state, &kernels, target, 0U);
	assert(error == EINVAL);
	view->level_count = 6U;

	/* Every object is destroyed through the wire and nothing stays allocated. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_SAMPLER, FIXTURE_SAMPLER);
	fixture_destroy(FIXTURE_DESTROY_IMAGE_VIEW, FIXTURE_VIEW);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_MIPMAPPED + 1U);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_MIPMAPPED);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 6U * 4U);
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A layout keeps its bindings, a set keeps its layout, and an update
 * records the view and the sampler a binding samples.
 */
static void
test_descriptors(void)
{
	struct i915_gfx_dsl *dsl;
	struct i915_gfx_dset *dset;
	struct i915_gfx_view *view;
	struct i915_gfx_sampler *sampler;
	size_t reply_bytes;
	unsigned index;
	int error;

	/* Opens a session with an image and its view to sample. */
	stub_session_open(NULL);
	stub_wire_begin(&fixture_wire);
	fixture_create_image(FIXTURE_IMAGE, VK_FORMAT_R8G8B8A8_UNORM, 16U, 16U, 1U);
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE_VIEW);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 15U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put32(&fixture_wire, VK_IMAGE_VIEW_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_R8G8B8A8_UNORM);
	for (index = 0U; index < 4U; index++)
		stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_VIEW);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 48U);

	/* vkCreateSampler: linear magnification, nearest minification, repeat along u, clamp along v. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_SAMPLER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 31U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_FILTER_LINEAR);
	stub_put32(&fixture_wire, VK_FILTER_NEAREST);
	stub_put32(&fixture_wire, VK_SAMPLER_MIPMAP_MODE_NEAREST);
	stub_put32(&fixture_wire, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	stub_put32(&fixture_wire, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	stub_put32(&fixture_wire, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	for (index = 0U; index < 9U; index++)
		stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_SAMPLER);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	sampler = drv_i915_object_lookup(stub_session, I915_VK_OBJ_SAMPLER, FIXTURE_SAMPLER);
	assert(sampler != NULL);
	assert(sampler->mag_filter == VK_FILTER_LINEAR);
	assert(sampler->min_filter == VK_FILTER_NEAREST);
	assert(sampler->address_u == VK_SAMPLER_ADDRESS_MODE_REPEAT);
	assert(sampler->address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

	/* vkCreateDescriptorSetLayout with one combined image sampler at binding 1, fragment stage. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_DSL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 32U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_SHADER_STAGE_FRAGMENT_BIT);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DSL);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	dsl = drv_i915_object_lookup(stub_session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, FIXTURE_DSL);
	assert(dsl != NULL);
	assert(dsl->count == 1U);
	assert(dsl->bindings[0].binding == 1U);
	assert(dsl->bindings[0].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	assert(dsl->bindings[0].stages == VK_SHADER_STAGE_FRAGMENT_BIT);

	/* A layout with more bindings than a set holds is refused as a missing feature. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_DSL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 32U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, I915_GFX_MAX_BINDINGS + 1U);
	stub_put64(&fixture_wire, I915_GFX_MAX_BINDINGS + 1U);
	for (index = 0U; index < I915_GFX_MAX_BINDINGS + 1U; index++) {
		stub_put32(&fixture_wire, index);
		stub_put32(&fixture_wire, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		stub_put32(&fixture_wire, 1U);
		stub_put32(&fixture_wire, VK_SHADER_STAGE_FRAGMENT_BIT);
		stub_put64(&fixture_wire, 0U);
	}
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_WIDE_DSL);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);

	/* vkCreateDescriptorPool for one set, then vkAllocateDescriptorSets of one set of the layout. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_DESCRIPTOR_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 33U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_DESCRIPTOR_SETS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 34U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DSL);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_SET);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 24U);
	assert(stub_get32(stub_reply, 24U) == FIXTURE_ALLOCATE_DESCRIPTOR_SETS);
	assert(stub_get32(stub_reply, 28U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 32U) == 1U);
	assert(stub_get64(stub_reply, 40U) == FIXTURE_SET);
	dset = drv_i915_object_lookup(stub_session, I915_VK_OBJ_DESCRIPTOR_SET, FIXTURE_SET);
	assert(dset != NULL);
	assert(dset->layout == dsl);

	/*
	 * vkUpdateDescriptorSets: one write of binding 1 with the view and the
	 * sampler, no buffers, no texel views, no copies.  The command has no
	 * reply body.
	 */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_UPDATE_DESCRIPTOR_SETS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 35U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_SET);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_SAMPLER);
	stub_put64(&fixture_wire, FIXTURE_VIEW);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	view = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE_VIEW, FIXTURE_VIEW);
	assert(dset->slots[1].view == view);
	assert(dset->slots[1].sampler == sampler);
	assert(dset->slots[0].view == NULL);

	/*
	 * vkFreeDescriptorSets is not implemented: its opcode is routed to the
	 * resource module that was not ported, and the stream is refused.
	 */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_FREE_DESCRIPTOR_SETS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_SET);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == ENOTSUP);
	assert(strstr(stub_log, "opcode 78 routed to the res module") != NULL);

	/*
	 * XXX: no command frees a descriptor set, and neither the pool's
	 * destruction nor the session's close does; the fixture unpublishes and
	 * frees the set itself so that the rest of the run starts clean.
	 */
	drv_i915_object_remove(stub_session, I915_VK_OBJ_DESCRIPTOR_SET, FIXTURE_SET);
	kern_free(dset);

	/* Every other object is destroyed through the wire and nothing stays allocated. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_DESCRIPTOR_POOL, FIXTURE_POOL);
	fixture_destroy(FIXTURE_DESTROY_DSL, FIXTURE_DSL);
	fixture_destroy(FIXTURE_DESTROY_SAMPLER, FIXTURE_SAMPLER);
	fixture_destroy(FIXTURE_DESTROY_IMAGE_VIEW, FIXTURE_VIEW);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 5U * 4U);
	stub_session_close();
	assert(stub_live == 0U);
}
