/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises all ordinary resource APIs through independent native wire parsing. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "internal.h"
#include "../../../userland/base/vkdemo/shaders.h"

#define TEST_OBJECTS 256U
#define TEST_ALLOCATIONS 256U
#define TEST_DEVICE_ID 1000U
#define TEST_PHYSICAL_ID 1001U
#define TEST_BUFFER_BYTES UINT64_C(0x100004000)

/* An independent cursor checks the wire without using production field decoders. */
struct peer_reader {
	const uint8_t *bytes;
	size_t size;
	size_t cursor;
};

/* Native object geometry and binding survive independently of local ownership records. */
struct peer_object {
	uint32_t kind;
	uint32_t flags;
	uint32_t format;
	uint32_t sharing;
	uint32_t family_count;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t mips;
	uint32_t layers;
	uint32_t samples;
	uint32_t tiling;
	uint32_t usage;
	uint64_t dependency;
	uint64_t bound_memory;
	uint64_t bound_offset;
};

/* The serial peer retains object state and bounded failure injection for complete calls. */
struct peer_state {
	struct peer_object objects[TEST_OBJECTS];
	uint32_t calls[85];
	uint32_t fail_opcode;
	uint32_t foreign_echo;
	VkResult failure;
	uint32_t render_final_layout;
	uint32_t render_attachment_count;
	uint32_t render_subpass_count;
	uint32_t sparse_total;
};

/* Compatible callback userdata remains distinct while using the same allocation mechanics. */
struct callback_state {
	unsigned live;
	unsigned object_frees;
	unsigned command_frees;
	unsigned fail_object;
};

/* Callback records retain creator accounting until the compatible destruction callback runs. */
struct callback_allocation {
	void *address;
	size_t bytes;
	VkSystemAllocationScope scope;
	struct callback_state *owner;
};

/* The fixture owns a native namespace independent of common local object parent lists. */
static struct peer_state peer;

/* Allocation records expose metadata leaks and wrong final-destroy userdata. */
static struct callback_allocation callback_allocations[TEST_ALLOCATIONS];

static uint32_t peer_u32(struct peer_reader *reader);
static uint64_t peer_u64(struct peer_reader *reader);
static void expect_u32(struct peer_reader *reader, uint32_t expected);
static void expect_u64(struct peer_reader *reader, uint64_t expected);
static void expect_structure(struct peer_reader *reader, VkStructureType structure);
static void *callback_allocate(void *userdata, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void *callback_reallocate(void *userdata, void *original, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void callback_free(void *userdata, void *address);
static void callbacks_init(VkAllocationCallbacks *callbacks, struct callback_state *state);
static struct callback_allocation *callback_find(void *address);
static uint64_t wire_id(uint64_t handle);
static unsigned child_count(const struct vulkan_object *parent);
static uint64_t peer_create(struct peer_reader *reader, uint32_t opcode, VkResult status);
static void peer_buffer(struct peer_reader *reader, struct peer_object *object);
static void peer_image(struct peer_reader *reader, struct peer_object *object);
static void peer_image_view(struct peer_reader *reader, struct peer_object *object);
static void peer_sampler(struct peer_reader *reader);
static void peer_render_pass(struct peer_reader *reader);
static void peer_subpass(struct peer_reader *reader, uint32_t index);
static void peer_sparse(struct peer_reader *reader, struct vulkan_writer *response, uint32_t opcode);
static void peer_sparse_property(struct vulkan_writer *response, uint32_t index);
static struct vulkan_object *fake_memory(struct VkDevice_T *device);
static void test_resources(struct VkDevice_T *device, struct VkPhysicalDevice_T *physical, void *guard);
static void test_render_failures(struct VkDevice_T *device, void *guard);
static void test_native_failures(void *guard);
static VkImage create_image(struct VkDevice_T *device, VkFormat format, VkImageCreateFlags flags, VkSharingMode sharing, void *guard);
static VkImageView create_image_view(struct VkDevice_T *device, VkImage image, VkFormat format);
static VkRenderPass create_render_pass(struct VkDevice_T *device, void *guard, const VkAllocationCallbacks *callbacks, VkResult *status);
static void test_sparse_queries(struct VkDevice_T *device, struct VkPhysicalDevice_T *physical, VkImage image, void *guard);
static void assert_empty(void);

/*
 * Exercises 24 core resource entry points and their observable native ownership.
 */
int
main(void)
{
	struct vulkan_context context;
	struct VkDevice_T device;
	struct VkPhysicalDevice_T physical;
	void *guard;
	unsigned index;
	uint32_t opcodes[24] = { 28U, 29U, 30U, 31U, 32U, 33U, 50U, 51U, 52U, 53U, 54U, 55U, 56U, 57U, 58U, 59U, 60U, 70U, 71U, 80U, 81U, 82U, 83U, 84U };
	int error;

	/* Only the surrounding device and physical-device namespace is substituted. */
	memset(&context, 0, sizeof(context));
	memset(&device, 0, sizeof(device));
	memset(&physical, 0, sizeof(physical));
	device.object.context = &context;
	device.object.wire_id = TEST_DEVICE_ID;
	physical.object.context = &context;
	physical.object.wire_id = TEST_PHYSICAL_ID;
	device.physical = &physical;
	peer.sparse_total = 2U;

	/* Inaccessible ignored pointers turn accidental dereferences into visible failures. */
	guard = mmap(NULL, 4096U, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(guard != MAP_FAILED);
	test_resources(&device, &physical, guard);
	test_render_failures(&device, guard);
	test_native_failures(guard);
	error = munmap(guard, 4096U);
	assert(error == 0);

	/* Local ownership and the independent native namespace must both be empty. */
	assert(device.object.first_child == NULL);
	assert(context.error == VK_SUCCESS);
	assert_empty();
	for (index = 0U; index < 24U; index++)
		assert(peer.calls[opcodes[index]] != 0U);

	/* Succeeded: every selected API crossed the independently checked native boundary. */
	puts("libvulkan resources: PASS (24 APIs, native geometry, sparse arrays, metadata, callbacks, loss)");
	return 0;
}

/*
 * Interprets actual resource requests without consulting production input codecs.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reply)
{
	struct peer_reader request;
	struct vulkan_writer response;
	struct peer_object *object;
	uint64_t identity;
	uint64_t memory;
	uint64_t offset;
	uint32_t opcode;
	uint32_t aspect;
	uint32_t index;
	VkResult status;

	/* A terminal namespace cannot accept another native command. */
	status = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* Request parsing uses an independent byte cursor and explicit native field widths. */
	request.bytes = writer->data;
	request.size = writer->bytes;
	request.cursor = 0U;
	opcode = peer_u32(&request);
	assert(opcode < 85U);
	expect_u32(&request, 1U);

	/* Physical sparse format queries use a physical-device identity instead of a logical device. */
	if (opcode == 33U) {
		expect_u64(&request, TEST_PHYSICAL_ID);
	} else {
		expect_u64(&request, TEST_DEVICE_ID);
	}

	/* Native error injection does not mutate previously accepted resource ownership. */
	peer.calls[opcode]++;
	status = VK_SUCCESS;
	if (peer.fail_opcode == opcode) {
		status = peer.failure;
		peer.fail_opcode = 0U;
	}

	/* Each reply owns independent bytes which the actual output decoder must consume. */
	vulkan_writer_init(&response);
	vulkan_write_u32(&response, opcode);

	/* Native object and output semantics are independent for every core resource operation. */
	switch (opcode) {
	case 50U:
	case 52U:
	case 54U:
	case 57U:
	case 59U:
	case 70U:
	case 80U:
	case 82U:
		/* Ordinary creates publish one native ID only on complete success. */
		identity = peer_create(&request, opcode, status);
		vulkan_write_u32(&response, (uint32_t)status);
		vulkan_write_u64(&response, 1U);

		/* A foreign successful output tests local refusal without pretending the host object vanished. */
		if (peer.foreign_echo != 0U) {
			peer.foreign_echo = 0U;
			identity++;
		}

		/* The wire echo is distinct from the application-visible local handle. */
		vulkan_write_u64(&response, identity);
		break;
	case 51U:
	case 53U:
	case 55U:
	case 58U:
	case 60U:
	case 71U:
	case 81U:
	case 83U:
		/* Void destruction consumes only the named native object. */
		identity = peer_u64(&request);
		expect_u64(&request, 0U);
		assert(identity < TEST_OBJECTS);
		assert(peer.objects[identity].kind != 0U);
		memset(&peer.objects[identity], 0, sizeof(peer.objects[identity]));
		break;
	case 28U:
	case 29U:
		/* Binding keeps the caller's full 64-bit memory identity and offset. */
		identity = peer_u64(&request);
		memory = peer_u64(&request);
		offset = peer_u64(&request);
		assert(identity < TEST_OBJECTS);
		assert(memory < TEST_OBJECTS);
		assert(peer.objects[memory].kind == 100U);
		object = &peer.objects[identity];
		assert(object->kind == 50U || object->kind == 54U);

		/* A rejected bind leaves the old native resource unbound. */
		if (status == VK_SUCCESS) {
			object->bound_memory = memory;
			object->bound_offset = offset;
		}

		/* Exact native errors are returned through the ordinary result field. */
		vulkan_write_u32(&response, (uint32_t)status);
		break;
	case 30U:
	case 31U:
		/* Requirements come from the peer and deliberately preserve sparse native memory type bits. */
		identity = peer_u64(&request);
		assert(identity < TEST_OBJECTS);
		assert(peer.objects[identity].kind != 0U);
		expect_u64(&request, 1U);
		vulkan_write_u64(&response, 1U);
		vulkan_write_u64(&response, UINT64_C(0x123450000));
		vulkan_write_u64(&response, 8192U);
		vulkan_write_u32(&response, UINT32_C(0x80000021));
		break;
	case 32U:
	case 33U:
		/* Sparse queries follow native NULL/count-only and bounded-array behavior exactly. */
		peer_sparse(&request, &response, opcode);
		break;
	case 56U:
		/* Linear image subresource queries preserve selected aspect, mip and layer. */
		identity = peer_u64(&request);
		assert(identity < TEST_OBJECTS);
		assert(peer.objects[identity].kind == 54U);
		expect_u64(&request, 1U);
		aspect = peer_u32(&request);
		assert(aspect == VK_IMAGE_ASPECT_COLOR_BIT);
		expect_u32(&request, 1U);
		expect_u32(&request, 2U);
		expect_u64(&request, 1U);
		vulkan_write_u64(&response, 1U);

		/* Every native pitch is deliberately distinct and larger than 32-bit scalar limits. */
		for (index = 0U; index < 5U; index++)
			vulkan_write_u64(&response, UINT64_C(0x100000000) + (index + 1U) * 4096U);

		/* Native subresource layout has no VkResult field. */
		break;
	case 84U:
		/* Render granularity belongs to this native render-pass implementation. */
		identity = peer_u64(&request);
		assert(identity < TEST_OBJECTS);
		assert(peer.objects[identity].kind == 82U);
		expect_u64(&request, 1U);
		vulkan_write_u64(&response, 1U);
		vulkan_write_u32(&response, 8U);
		vulkan_write_u32(&response, 16U);
		break;
	default:
		/* This fixture cannot accidentally succeed an unimplemented resource operation. */
		fprintf(stderr, "unexpected resource opcode %u\n", opcode);
		abort();
	}

	/* Exact consumption rejects padding mistakes, omitted fields and mismatched array cardinality. */
	assert(request.cursor == request.size);
	assert(response.error == VK_SUCCESS);
	assert(response.bytes <= reply_capacity);
	vulkan_reader_init(reply, response.data, response.bytes);

	/* Succeeded: native API failure, when present, lives only in the reply's VkResult field. */
	return VK_SUCCESS;
}

/* Parses an ordinary native creation without interpreting application input pointers. */
static uint64_t
peer_create(
	struct peer_reader *reader,
	uint32_t opcode,
	VkResult status)
{
	struct peer_object parsed;
	uint64_t identity;
	uint64_t bytes;
	uint64_t count;
	const uint32_t *words;
	uint32_t scalar;
	uint32_t index;

	/* The required create-info pointer precedes one complete typed structure. */
	memset(&parsed, 0, sizeof(parsed));
	expect_u64(reader, 1U);

	/* Every supported resource has its own independently parsed native structure. */
	switch (opcode) {
	case 50U:
		/* Buffer sharing mode decides whether queue-family storage is active. */
		peer_buffer(reader, &parsed);
		break;
	case 52U:
		/* Buffer views preserve their source and full-width byte range. */
		expect_structure(reader, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
		expect_u32(reader, 0U);
		parsed.dependency = peer_u64(reader);
		assert(parsed.dependency < TEST_OBJECTS);
		assert(peer.objects[parsed.dependency].kind == 50U);
		expect_u32(reader, VK_FORMAT_R32_UINT);
		expect_u64(reader, 4096U);
		expect_u64(reader, 8192U);
		break;
	case 54U:
		/* Native image parameters remain independent of retained local image metadata. */
		peer_image(reader, &parsed);
		break;
	case 57U:
		/* Image views reference a real created image and preserve swizzle/subresource selection. */
		peer_image_view(reader, &parsed);
		break;
	case 59U:
		/* The original independently tested shaders exercise byte-size versus word-count framing. */
		expect_structure(reader, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
		expect_u32(reader, 0U);
		bytes = peer_u64(reader);
		count = peer_u64(reader);
		assert(bytes == count * 4U);
		words = vkdemo_vertex_shader;

		/* The fixture creates both original modules, which have distinct immutable lengths. */
		if (bytes == sizeof(vkdemo_fragment_shader)) {
			words = vkdemo_fragment_shader;
		} else {
			assert(bytes == sizeof(vkdemo_vertex_shader));
		}

		/* Every encoded word must preserve the original SPIR-V module bit pattern. */
		for (index = 0U; index < count; index++) {
			scalar = peer_u32(reader);
			assert(scalar == words[index]);
		}

		/* The native peer owns the resulting module identity after the output trailer. */
		break;
	case 70U:
		/* Sampler fields contain both scalar enums and exact IEEE float bits. */
		peer_sampler(reader);
		break;
	case 80U:
		/* Framebuffer creation retains the native render pass and two actual image-view handles. */
		expect_structure(reader, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
		expect_u32(reader, 0U);
		parsed.dependency = peer_u64(reader);
		assert(parsed.dependency < TEST_OBJECTS);
		assert(peer.objects[parsed.dependency].kind == 82U);
		expect_u32(reader, 2U);
		expect_u64(reader, 2U);
		for (index = 0U; index < 2U; index++) {
			identity = peer_u64(reader);
			assert(identity < TEST_OBJECTS);
			assert(peer.objects[identity].kind == 57U);
		}

		/* All attachments fit these finite framebuffer dimensions. */
		expect_u32(reader, 128U);
		expect_u32(reader, 64U);
		expect_u32(reader, 1U);
		break;
	case 82U:
		/* Render-pass metadata and the native layout translation are checked separately. */
		peer_render_pass(reader);
		break;
	default:
		/* An unrecognized creation must never be accepted by this peer. */
		abort();
	}

	/* Native allocators remain NULL while local callback-owned outputs receive fresh IDs. */
	expect_u64(reader, 0U);
	expect_u64(reader, 1U);
	identity = peer_u64(reader);
	assert(identity < TEST_OBJECTS);
	assert(peer.objects[identity].kind == 0U);

	/* No remote ownership is created on an ordinary native VkResult failure. */
	if (status == VK_SUCCESS) {
		parsed.kind = opcode;
		peer.objects[identity] = parsed;
	}

	/* Succeeded: this reserved identity belongs in the reply's output handle field. */
	return identity;
}

/* Checks ordinary buffer geometry and whether queue-family arrays are active. */
static void
peer_buffer(
	struct peer_reader *reader,
	struct peer_object *object)
{
	uint64_t count;

	/* The test's large buffer exposes truncation of VkDeviceSize inputs. */
	expect_structure(reader, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
	expect_u32(reader, 0U);
	expect_u64(reader, TEST_BUFFER_BYTES);
	expect_u32(reader, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
	object->sharing = peer_u32(reader);
	object->family_count = peer_u32(reader);
	count = peer_u64(reader);

	/* An exclusive buffer must not dereference its ignored nonzero-count guard pointer. */
	if (object->sharing == VK_SHARING_MODE_EXCLUSIVE) {
		assert(object->family_count == 3U);
		assert(count == 0U);
	} else {
		assert(object->family_count == 2U);
		assert(count == 2U);
		expect_u32(reader, 3U);
		expect_u32(reader, 7U);
	}

	/* Succeeded: buffer geometry and the selected native array are fully consumed. */
	return;
}

/* Checks full image geometry without relying on the local subtype fields. */
static void
peer_image(
	struct peer_reader *reader,
	struct peer_object *object)
{
	uint64_t count;
	uint32_t initial;

	/* The image record includes all dimensions, levels, layers, samples and usage. */
	expect_structure(reader, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	object->flags = peer_u32(reader);
	expect_u32(reader, VK_IMAGE_TYPE_2D);
	object->format = peer_u32(reader);
	object->width = peer_u32(reader);
	object->height = peer_u32(reader);
	object->depth = peer_u32(reader);
	object->mips = peer_u32(reader);
	object->layers = peer_u32(reader);
	object->samples = peer_u32(reader);
	object->tiling = peer_u32(reader);
	object->usage = peer_u32(reader);
	object->sharing = peer_u32(reader);
	object->family_count = peer_u32(reader);
	count = peer_u64(reader);
	assert(object->width == 128U);
	assert(object->height == 64U);
	assert(object->depth == 1U);
	assert(object->samples == VK_SAMPLE_COUNT_1_BIT);

	/* Concurrent ownership preserves both native queue-family indices in declared order. */
	if (object->sharing == VK_SHARING_MODE_CONCURRENT) {
		assert(object->family_count == 2U);
		assert(count == 2U);
		expect_u32(reader, 3U);
		expect_u32(reader, 7U);
	} else {
		assert(object->family_count == 3U);
		assert(count == 0U);
	}

	/* Linear color images support the subsequent subresource-layout query. */
	initial = peer_u32(reader);
	if (object->format == VK_FORMAT_R8G8B8A8_UNORM) {
		/* Sparse color images use optimal storage with one level and layer. */
		if (object->flags != 0U) {
			assert(object->mips == 1U);
			assert(object->layers == 1U);
			assert(object->tiling == VK_IMAGE_TILING_OPTIMAL);
			assert(initial == VK_IMAGE_LAYOUT_UNDEFINED);
			return;
		}

		/* Ordinary color images retain the linear subresource query geometry. */
		assert(object->mips == 3U);
		assert(object->layers == 4U);
		assert(object->tiling == VK_IMAGE_TILING_LINEAR);
		assert(initial == VK_IMAGE_LAYOUT_PREINITIALIZED);
	} else {
		assert(object->mips == 1U);
		assert(object->layers == 1U);
		assert(object->tiling == VK_IMAGE_TILING_OPTIMAL);
		assert(initial == VK_IMAGE_LAYOUT_UNDEFINED);
	}

	/* Succeeded: every standard image-create field has an independently checked representation. */
	return;
}

/* Checks view dependencies, component swizzles and the selected image subresource range. */
static void
peer_image_view(
	struct peer_reader *reader,
	struct peer_object *object)
{
	uint32_t aspect;

	/* A view must reference a live native image and use its actual format. */
	expect_structure(reader, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
	expect_u32(reader, 0U);
	object->dependency = peer_u64(reader);
	assert(object->dependency < TEST_OBJECTS);
	assert(peer.objects[object->dependency].kind == 54U);
	expect_u32(reader, VK_IMAGE_VIEW_TYPE_2D);
	object->format = peer_u32(reader);
	assert(object->format == peer.objects[object->dependency].format);
	expect_u32(reader, VK_COMPONENT_SWIZZLE_IDENTITY);
	expect_u32(reader, VK_COMPONENT_SWIZZLE_IDENTITY);
	expect_u32(reader, VK_COMPONENT_SWIZZLE_IDENTITY);
	expect_u32(reader, VK_COMPONENT_SWIZZLE_IDENTITY);
	aspect = peer_u32(reader);

	/* Color and depth views select different actual image aspects. */
	if (object->format == VK_FORMAT_R8G8B8A8_UNORM) {
		assert(aspect == VK_IMAGE_ASPECT_COLOR_BIT);
	} else {
		assert(aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
	}

	/* Framebuffer views select one level and layer from their complete underlying image. */
	expect_u32(reader, 0U);
	expect_u32(reader, 1U);
	expect_u32(reader, 0U);
	expect_u32(reader, 1U);

	/* Succeeded: the view dependency and every subresource field are preserved. */
	return;
}

/* Checks exact sampler enum and floating-point bit representations. */
static void
peer_sampler(
	struct peer_reader *reader)
{
	/* These finite, distinct standard values expose field-order and float-width mistakes. */
	expect_structure(reader, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
	expect_u32(reader, 0U);
	expect_u32(reader, VK_FILTER_LINEAR);
	expect_u32(reader, VK_FILTER_NEAREST);
	expect_u32(reader, VK_SAMPLER_MIPMAP_MODE_LINEAR);
	expect_u32(reader, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	expect_u32(reader, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	expect_u32(reader, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
	expect_u32(reader, UINT32_C(0x3e800000));
	expect_u32(reader, VK_FALSE);
	expect_u32(reader, UINT32_C(0x3f800000));
	expect_u32(reader, VK_TRUE);
	expect_u32(reader, VK_COMPARE_OP_LESS_OR_EQUAL);
	expect_u32(reader, UINT32_C(0x3f000000));
	expect_u32(reader, UINT32_C(0x40800000));
	expect_u32(reader, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
	expect_u32(reader, VK_FALSE);

	/* Succeeded: sampler behavior can be reconstructed from the exact native values. */
	return;
}

/* Parses attachment records and subpasses while observing native PRESENT-to-GENERAL translation. */
static void
peer_render_pass(
	struct peer_reader *reader)
{
	uint32_t index;

	/* Two real attachment formats exercise color and depth metadata independently. */
	expect_structure(reader, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
	expect_u32(reader, 0U);
	expect_u32(reader, 2U);
	expect_u64(reader, 2U);
	peer.render_attachment_count = 2U;
	for (index = 0U; index < 2U; index++) {
		expect_u32(reader, 0U);

		/* Each attachment retains the corresponding created view's format. */
		if (index == 0U) {
			expect_u32(reader, VK_FORMAT_R8G8B8A8_UNORM);
		} else {
			expect_u32(reader, VK_FORMAT_D32_SFLOAT);
		}

		/* Load/store operations remain independent of native layout translation. */
		expect_u32(reader, VK_SAMPLE_COUNT_1_BIT);
		expect_u32(reader, VK_ATTACHMENT_LOAD_OP_CLEAR);
		expect_u32(reader, VK_ATTACHMENT_STORE_OP_STORE);
		expect_u32(reader, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		expect_u32(reader, VK_ATTACHMENT_STORE_OP_DONT_CARE);
		expect_u32(reader, VK_IMAGE_LAYOUT_UNDEFINED);

		/* Presentation is a guest WSI layout; the native host sees preserved GENERAL. */
		if (index == 0U) {
			peer.render_final_layout = peer_u32(reader);
			assert(peer.render_final_layout == VK_IMAGE_LAYOUT_GENERAL);
		} else {
			expect_u32(reader, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		}
	}

	/* Every subpass declares its own active color and depth/stencil usage. */
	expect_u32(reader, 3U);
	expect_u64(reader, 3U);
	peer.render_subpass_count = 3U;
	for (index = 0U; index < 3U; index++)
		peer_subpass(reader, index);

	/* This fixture needs no explicit cross-subpass dependency input records. */
	expect_u32(reader, 0U);
	expect_u64(reader, 0U);

	/* Succeeded: native attachment and subpass arrays preserve complete framing. */
	return;
}

/* Distinguishes used and UNUSED attachment references in one native subpass. */
static void
peer_subpass(
	struct peer_reader *reader,
	uint32_t index)
{
	/* Empty input and preserve arrays must not dereference their guarded source pointers. */
	expect_u32(reader, 0U);
	expect_u32(reader, VK_PIPELINE_BIND_POINT_GRAPHICS);
	expect_u32(reader, 0U);
	expect_u64(reader, 0U);
	expect_u32(reader, 1U);
	expect_u64(reader, 1U);

	/* The middle subpass ignores its color reference, while the others use attachment zero. */
	if (index == 1U) {
		expect_u32(reader, VK_ATTACHMENT_UNUSED);
		expect_u32(reader, VK_IMAGE_LAYOUT_UNDEFINED);
	} else {
		expect_u32(reader, 0U);
		expect_u32(reader, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}

	/* No resolve attachment array was requested. */
	expect_u64(reader, 0U);

	/* The last subpass has no depth pointer; the middle pointer explicitly names UNUSED. */
	if (index == 2U) {
		expect_u64(reader, 0U);
	} else {
		expect_u64(reader, 1U);

		/* Depth is active only in the first subpass. */
		if (index == 0U) {
			expect_u32(reader, 1U);
			expect_u32(reader, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		} else {
			expect_u32(reader, VK_ATTACHMENT_UNUSED);
			expect_u32(reader, VK_IMAGE_LAYOUT_UNDEFINED);
		}
	}

	/* A zero preserve count suppresses its inaccessible pointer on the wire. */
	expect_u32(reader, 0U);
	expect_u64(reader, 0U);

	/* Succeeded: active graphics state can be derived independently from the native subpass. */
	return;
}

/* Models the pinned renderer's NULL/count-only and bounded sparse array behavior. */
static void
peer_sparse(
	struct peer_reader *reader,
	struct vulkan_writer *response,
	uint32_t opcode)
{
	uint64_t identity;
	uint64_t array;
	uint32_t capacity;
	uint32_t count;
	uint32_t index;

	/* Image requirements and physical format queries begin with distinct core inputs. */
	if (opcode == 32U) {
		identity = peer_u64(reader);
		assert(identity < TEST_OBJECTS);
		assert(peer.objects[identity].kind == 54U);
		assert((peer.objects[identity].flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0U);
	} else {
		expect_u32(reader, VK_FORMAT_R8G8B8A8_UNORM);
		expect_u32(reader, VK_IMAGE_TYPE_2D);
		expect_u32(reader, VK_SAMPLE_COUNT_1_BIT);
		expect_u32(reader, VK_IMAGE_USAGE_SAMPLED_BIT);
		expect_u32(reader, VK_IMAGE_TILING_OPTIMAL);
	}

	/* Array marker zero is NULL in the actual native decoder, even with a non-NULL guest pointer. */
	expect_u64(reader, 1U);
	capacity = peer_u32(reader);
	array = peer_u64(reader);
	assert(array == capacity);
	count = peer.sparse_total;

	/* Native bounded enumeration writes no more than the provided capacity. */
	if (array != 0U) {
		if (count > capacity)
			count = capacity;
	}

	/* Count-only calls report total support and carry no output array records. */
	vulkan_write_u64(response, 1U);
	vulkan_write_u32(response, count);
	if (array == 0U) {
		vulkan_write_u64(response, 0U);
		return;
	}

	/* Every returned record has the exact standard fields and protocol widths. */
	vulkan_write_u64(response, count);
	for (index = 0U; index < count; index++) {
		peer_sparse_property(response, index);

		/* Image-specific requirements append mip-tail geometry to the common format properties. */
		if (opcode == 32U) {
			vulkan_write_u32(response, 3U + index);
			vulkan_write_u64(response, UINT64_C(0x100000000) + index);
			vulkan_write_u64(response, UINT64_C(0x200000000) + index);
			vulkan_write_u64(response, 4096U + index);
		}
	}

	/* Succeeded: returned count denotes complete records independently of their input capacity. */
	return;
}

/* Encodes independently chosen sparse geometry without using a production structure encoder. */
static void
peer_sparse_property(
	struct vulkan_writer *response,
	uint32_t index)
{
	/* Distinct components expose field transposition and incorrect fixed record sizes. */
	vulkan_write_u32(response, VK_IMAGE_ASPECT_COLOR_BIT);
	vulkan_write_u32(response, 16U + index);
	vulkan_write_u32(response, 32U + index);
	vulkan_write_u32(response, 1U);
	vulkan_write_u32(response, VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT);

	/* Succeeded: one complete twenty-byte property record follows the count marker. */
	return;
}

/* Reads one little-endian scalar without using the production wire helpers. */
static uint32_t
peer_u32(
	struct peer_reader *reader)
{
	uint32_t scalar;
	unsigned index;

	/* Every scalar must fit completely within this one command's bounded stream. */
	assert(reader->cursor <= reader->size);
	assert(reader->size - reader->cursor >= 4U);
	scalar = 0U;
	for (index = 0U; index < 4U; index++)
		scalar |= (uint32_t)reader->bytes[reader->cursor + index] << (index * 8U);

	/* This field consumes exactly four bytes, independently of native structure padding. */
	reader->cursor += 4U;

	/* Succeeded: the scalar retains the little-endian protocol bit pattern. */
	return scalar;
}

/* Reads one independent 64-bit identity, count or device-size field. */
static uint64_t
peer_u64(
	struct peer_reader *reader)
{
	uint64_t scalar;
	unsigned index;

	/* No field may cross the end of this request's owned stream. */
	assert(reader->cursor <= reader->size);
	assert(reader->size - reader->cursor >= 8U);
	scalar = 0U;
	for (index = 0U; index < 8U; index++)
		scalar |= (uint64_t)reader->bytes[reader->cursor + index] << (index * 8U);

	/* The protocol's pointer and array markers occupy eight bytes. */
	reader->cursor += 8U;

	/* Succeeded: the peer has consumed this exact 64-bit field. */
	return scalar;
}

/* Separates scalar decoding from its independently declared expected meaning. */
static void
expect_u32(
	struct peer_reader *reader,
	uint32_t expected)
{
	uint32_t observed;

	/* A discrepancy identifies the precise field boundary in the native request. */
	observed = peer_u32(reader);
	if (observed != expected) {
		fprintf(stderr, "wire32 at %lu: expected=%u observed=%u\n", (unsigned long)(reader->cursor - 4U), expected, observed);
		abort();
	}

	/* Succeeded: this field matches the fixture's independent protocol expectation. */
	return;
}

/* Separates 64-bit native identity and array cardinality expectations. */
static void
expect_u64(
	struct peer_reader *reader,
	uint64_t expected)
{
	uint64_t observed;

	/* The peer compares wire values without converting application handle pointers. */
	observed = peer_u64(reader);
	if (observed != expected) {
		fprintf(stderr, "wire64 at %lu: expected=%lu observed=%lu\n", (unsigned long)(reader->cursor - 8U), (unsigned long)expected, (unsigned long)observed);
		abort();
	}

	/* Succeeded: this identity or framing marker has the expected width and content. */
	return;
}

/* Checks an ordinary Vulkan1.0 structure header with no enabled extension chain. */
static void
expect_structure(
	struct peer_reader *reader,
	VkStructureType structure)
{
	/* The selected public extensions add no pNext record to these core structures. */
	expect_u32(reader, structure);
	expect_u64(reader, 0U);

	/* Succeeded: the next byte begins the core structure's ordinary fields. */
	return;
}

/* Allocates callback storage with a targeted object-only failure countdown. */
static void *
callback_allocate(
	void *userdata,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	struct callback_state *state;
	struct callback_allocation *record;
	void *allocation;
	unsigned index;
	int error;

	/* Resource objects and their command temporaries must use the effective policy. */
	state = userdata;
	assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT || scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);

	/* Fail a later metadata preparation without affecting earlier callback ownership. */
	if (scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) {
		/* A disabled failure counter leaves ordinary object allocation unchanged. */
		if (state->fail_object != 0U) {
			state->fail_object--;

			/* The selected object acquires no host storage. */
			if (state->fail_object == 0U)
				return NULL;
		}
	}

	/* Host allocation respects the Vulkan-requested alignment. */
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);

	/* Every returned pointer is tracked independently of the production object list. */
	error = posix_memalign(&allocation, alignment, bytes);
	if (error != 0)
		return NULL;

	/* A free callback record retains creator accounting even under compatible destroy userdata. */
	record = NULL;
	for (index = 0U; index < TEST_ALLOCATIONS; index++) {
		if (callback_allocations[index].address == NULL) {
			record = &callback_allocations[index];
			break;
		}
	}

	/* The finite fixture has ample records for its maximum simultaneous batch. */
	assert(record != NULL);
	record->address = allocation;
	record->bytes = bytes;
	record->scope = scope;
	record->owner = state;
	state->live++;

	/* Succeeded: the library owns one tracked callback allocation. */
	return allocation;
}

/* Preserves ordinary callback reallocation ownership if a shared helper requests it. */
static void *
callback_reallocate(
	void *userdata,
	void *original,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	struct callback_allocation *record;
	void *fresh;
	size_t copied;

	/* A zero-size replacement consumes the original allocation. */
	if (bytes == 0U) {
		callback_free(userdata, original);
		return NULL;
	}

	/* Failed reallocation leaves the caller's original allocation intact. */
	fresh = callback_allocate(userdata, bytes, alignment, scope);
	if (fresh == NULL)
		return NULL;

	/* Existing data survives up to the smaller allocation's extent. */
	if (original != NULL) {
		record = callback_find(original);
		assert(record != NULL);
		copied = record->bytes;

		/* Shrinking cannot copy beyond the newly requested storage. */
		if (copied > bytes)
			copied = bytes;

		/* The new allocation owns the preserved bytes before the old one retires. */
		memcpy(fresh, original, copied);
		callback_free(userdata, original);
	}

	/* Succeeded: only the replacement allocation remains owned by the caller. */
	return fresh;
}

/* Frees callback storage while recording which compatible userdata received destruction. */
static void
callback_free(
	void *userdata,
	void *address)
{
	struct callback_state *state;
	struct callback_allocation *record;

	/* Vulkan permits callbacks to receive a null cleanup pointer. */
	if (address == NULL)
		return;

	/* A missing record would be a duplicate or foreign allocator release. */
	state = userdata;
	record = callback_find(address);
	assert(record != NULL);
	assert(record->owner->live != 0U);
	record->owner->live--;

	/* Record destruction userdata separately for host objects and command temporaries. */
	if (record->scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) {
		state->object_frees++;
	} else {
		state->command_frees++;
	}

	/* No callback record can continue referring to the released allocation. */
	memset(record, 0, sizeof(*record));
	free(address);

	/* Succeeded: creator accounting and destructor userdata are both observable. */
	return;
}

/* Initializes the application's ordinary callback set with distinct fixture userdata. */
static void
callbacks_init(
	VkAllocationCallbacks *callbacks,
	struct callback_state *state)
{
	/* A complete valid callback set supports allocations, reallocations and frees. */
	memset(callbacks, 0, sizeof(*callbacks));
	callbacks->pUserData = state;
	callbacks->pfnAllocation = callback_allocate;
	callbacks->pfnReallocation = callback_reallocate;
	callbacks->pfnFree = callback_free;

	/* Succeeded: this policy can be supplied to any ordinary resource creation. */
	return;
}

/* Finds a callback allocation without inspecting memory which may already be freed. */
static struct callback_allocation *
callback_find(
	void *address)
{
	unsigned index;

	/* Every live callback pointer appears once in the finite fixture registry. */
	for (index = 0U; index < TEST_ALLOCATIONS; index++) {
		if (callback_allocations[index].address == address)
			return &callback_allocations[index];
	}

	/* A missing record denotes a foreign or already-consumed allocation. */
	return NULL;
}

/* Resolves a public non-dispatchable handle only when constructing an independent expectation. */
static uint64_t
wire_id(
	uint64_t handle)
{
	struct vulkan_object *object;

	/* Native peer parsing never consults this local handle conversion. */
	object = vulkan_nondispatchable_object(handle);
	assert(object != NULL);

	/* Succeeded: tests can compare the peer's observed identity with the intended resource. */
	return object->wire_id;
}

/* Counts real local child ownership without interpreting private resource subtype storage. */
static unsigned
child_count(
	const struct vulkan_object *parent)
{
	const struct vulkan_object *child;
	unsigned count;

	/* The fixture has no concurrent mutation of these externally synchronized parents. */
	count = 0U;
	for (child = parent->first_child; child != NULL; child = child->next_sibling)
		count++;

	/* Succeeded: each remaining child owns one live local object allocation. */
	return count;
}

/* Creates only the surrounding memory identity; the real resource bind API uses it normally. */
static struct vulkan_object *
fake_memory(
	struct VkDevice_T *device)
{
	struct vulkan_object *object;
	VkResult status;

	/* This namespace fixture does not substitute any resource API implementation. */
	status = vulkan_object_alloc(sizeof(*object), __alignof__(struct vulkan_object), VULKAN_OBJECT_DEVICE_MEMORY, &device->object, device->object.context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	assert(status == VK_SUCCESS);
	status = vulkan_object_reserve_id(object);
	assert(status == VK_SUCCESS);
	status = vulkan_object_publish(object);
	assert(status == VK_SUCCESS);
	assert(object->wire_id < TEST_OBJECTS);
	peer.objects[object->wire_id].kind = 100U;

	/* Succeeded: the native memory ID exists independently from each resource binding. */
	return object;
}

/* Creates distinct linear color, optimal depth and sparse images through the public API. */
static VkImage
create_image(
	struct VkDevice_T *device,
	VkFormat format,
	VkImageCreateFlags flags,
	VkSharingMode sharing,
	void *guard)
{
	VkImageCreateInfo info;
	VkImage image;
	VkResult status;
	uint32_t families[2] = { 3U, 7U };
	struct vulkan_image *local;

	/* Complete geometry is checked independently by the peer and retained local subtype. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.flags = flags;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = 128U;
	info.extent.height = 64U;
	info.extent.depth = 1U;
	info.mipLevels = 1U;
	info.arrayLayers = 1U;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode = sharing;
	info.queueFamilyIndexCount = 3U;
	info.pQueueFamilyIndices = guard;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/* The active concurrent array preserves distinct native queue-family indices. */
	if (sharing == VK_SHARING_MODE_CONCURRENT) {
		info.queueFamilyIndexCount = 2U;
		info.pQueueFamilyIndices = families;
	}

	/* The linear image has several levels and layers for its subresource-layout query. */
	if (flags == 0U) {
		if (format == VK_FORMAT_R8G8B8A8_UNORM) {
			info.mipLevels = 3U;
			info.arrayLayers = 4U;
			info.tiling = VK_IMAGE_TILING_LINEAR;
			info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
			info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
	}

	/* Depth storage has the usage required by its later framebuffer attachment. */
	if (format == VK_FORMAT_D32_SFLOAT)
		info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Actual creation publishes one local typed object only after the native success. */
	image = VK_NULL_HANDLE;
	status = vkCreateImage(device, &info, NULL, &image);
	assert(status == VK_SUCCESS);
	local = vulkan_image(image);
	assert(local != NULL);
	assert(local->format == info.format);
	assert(local->type == info.imageType);
	assert(local->extent.width == 128U);
	assert(local->extent.height == 64U);
	assert(local->extent.depth == 1U);
	assert(local->mip_levels == info.mipLevels);
	assert(local->array_layers == info.arrayLayers);
	assert(local->samples == info.samples);
	assert(local->usage == info.usage);
	assert(local->tiling == info.tiling);
	assert(local->sharing_mode == info.sharingMode);

	/* Succeeded: callers receive a real API image with independently verified metadata. */
	return image;
}

/* Creates a framebuffer view with distinct color or depth aspect selection. */
static VkImageView
create_image_view(
	struct VkDevice_T *device,
	VkImage image,
	VkFormat format)
{
	VkImageViewCreateInfo info;
	VkImageView view;
	VkResult status;

	/* Identity swizzles and a single level/layer preserve the actual backing image format. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image = image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = format;
	info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	info.subresourceRange.levelCount = 1U;
	info.subresourceRange.layerCount = 1U;

	/* Depth attachments cannot use the color aspect. */
	if (format == VK_FORMAT_D32_SFLOAT)
		info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

	/* The actual encoder must resolve the local image into its native wire identity. */
	view = VK_NULL_HANDLE;
	status = vkCreateImageView(device, &info, NULL, &view);
	assert(status == VK_SUCCESS);

	/* Succeeded: this live view can be referenced by the ordinary framebuffer creation. */
	return view;
}

/* Exercises copied attachment metadata, unused references and native presentation translation. */
static VkRenderPass
create_render_pass(
	struct VkDevice_T *device,
	void *guard,
	const VkAllocationCallbacks *callbacks,
	VkResult *status)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference colors[3];
	VkAttachmentReference depths[2];
	VkSubpassDescription subpasses[3];
	VkRenderPassCreateInfo info;
	VkRenderPass pass;
	const VkAttachmentDescription *retained;
	VkBool32 color;
	VkBool32 depth;
	VkResult query_status;
	uint32_t index;

	/* Each attachment has meaningful format, load/store and final-layout metadata. */
	memset(attachments, 0, sizeof(attachments));
	for (index = 0U; index < 2U; index++) {
		attachments[index].format = VK_FORMAT_R8G8B8A8_UNORM;
		attachments[index].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[index].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}

	/* Depth uses its native optimal layout, while presentation is translated only on the wire. */
	attachments[1].format = VK_FORMAT_D32_SFLOAT;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(subpasses, 0, sizeof(subpasses));
	for (index = 0U; index < 3U; index++) {
		colors[index].attachment = 0U;
		colors[index].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		subpasses[index].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpasses[index].pInputAttachments = guard;
		subpasses[index].colorAttachmentCount = 1U;
		subpasses[index].pColorAttachments = &colors[index];
		subpasses[index].pPreserveAttachments = guard;
	}

	/* The middle subpass has non-NULL references whose attachments are explicitly unused. */
	colors[1].attachment = VK_ATTACHMENT_UNUSED;
	colors[1].layout = VK_IMAGE_LAYOUT_UNDEFINED;
	depths[0].attachment = 1U;
	depths[0].layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depths[1].attachment = VK_ATTACHMENT_UNUSED;
	depths[1].layout = VK_IMAGE_LAYOUT_UNDEFINED;
	subpasses[0].pDepthStencilAttachment = &depths[0];
	subpasses[1].pDepthStencilAttachment = &depths[1];
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 2U;
	info.pAttachments = attachments;
	info.subpassCount = 3U;
	info.pSubpasses = subpasses;
	info.pDependencies = guard;
	pass = VK_NULL_HANDLE;
	*status = vkCreateRenderPass(device, &info, callbacks, &pass);

	/* Failed local or native creation cannot publish a partial metadata owner. */
	if (*status != VK_SUCCESS) {
		assert(pass == VK_NULL_HANDLE);
		return VK_NULL_HANDLE;
	}

	/* Subpass summaries distinguish an unused attachment from a merely non-NULL pointer. */
	query_status = vulkan_render_pass_subpass(pass, 0U, &color, &depth);
	assert(query_status == VK_SUCCESS);
	assert(color == VK_TRUE);
	assert(depth == VK_TRUE);
	query_status = vulkan_render_pass_subpass(pass, 1U, &color, &depth);
	assert(query_status == VK_SUCCESS);
	assert(color == VK_FALSE);
	assert(depth == VK_FALSE);
	query_status = vulkan_render_pass_subpass(pass, 2U, &color, &depth);
	assert(query_status == VK_SUCCESS);
	assert(color == VK_TRUE);
	assert(depth == VK_FALSE);

	/* Caller metadata can be overwritten immediately after creation without changing the retained copy. */
	memset(attachments, 0xa5, sizeof(attachments));
	retained = vulkan_render_pass_attachment(pass, 0U);
	assert(retained != NULL);
	assert(retained->format == VK_FORMAT_R8G8B8A8_UNORM);
	assert(retained->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(retained->finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	retained = vulkan_render_pass_attachment(pass, 1U);
	assert(retained != NULL);
	assert(retained->format == VK_FORMAT_D32_SFLOAT);
	retained = vulkan_render_pass_attachment(pass, 2U);
	assert(retained == NULL);

	/* Succeeded: ordinary render-pass creation owns every metadata byte consumed later by commands. */
	return pass;
}

/* Verifies sparse count-only, bounded, complete, empty and zero-capacity enumeration semantics. */
static void
test_sparse_queries(
	struct VkDevice_T *device,
	struct VkPhysicalDevice_T *physical,
	VkImage image,
	void *guard)
{
	VkSparseImageMemoryRequirements requirements[3];
	VkSparseImageFormatProperties properties[3];
	uint32_t count;
	uint32_t index;

	/* Count-only queries ignore the input count and return the native total. */
	count = UINT32_MAX;
	vkGetImageSparseMemoryRequirements(device, image, &count, NULL);
	assert(count == 2U);
	count = UINT32_MAX;
	vkGetPhysicalDeviceSparseImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, NULL);
	assert(count == 2U);

	/* A one-entry output must preserve the untouched following entry and return only the written count. */
	memset(requirements, 0xa5, sizeof(requirements));
	memset(properties, 0xa5, sizeof(properties));
	count = 1U;
	vkGetImageSparseMemoryRequirements(device, image, &count, requirements);
	assert(count == 1U);
	assert(requirements[1].formatProperties.aspectMask == UINT32_C(0xa5a5a5a5));
	count = 1U;
	vkGetPhysicalDeviceSparseImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, properties);
	assert(count == 1U);
	assert(properties[1].aspectMask == UINT32_C(0xa5a5a5a5));

	/* Complete outputs preserve every sparse geometry field, including three 64-bit tails. */
	count = 2U;
	vkGetImageSparseMemoryRequirements(device, image, &count, requirements);
	assert(count == 2U);
	count = 2U;
	vkGetPhysicalDeviceSparseImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, properties);
	assert(count == 2U);
	for (index = 0U; index < 2U; index++) {
		assert(properties[index].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
		assert(properties[index].imageGranularity.width == 16U + index);
		assert(properties[index].imageGranularity.height == 32U + index);
		assert(properties[index].imageGranularity.depth == 1U);
		assert(properties[index].flags == VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT);
		assert(requirements[index].formatProperties.imageGranularity.width == 16U + index);
		assert(requirements[index].imageMipTailFirstLod == 3U + index);
		assert(requirements[index].imageMipTailSize == UINT64_C(0x100000000) + index);
		assert(requirements[index].imageMipTailOffset == UINT64_C(0x200000000) + index);
		assert(requirements[index].imageMipTailStride == 4096U + index);
	}

	/* Unsupported combinations return zero without touching any output record. */
	peer.sparse_total = 0U;
	count = 2U;
	vkGetImageSparseMemoryRequirements(device, image, &count, requirements);
	assert(count == 0U);
	count = 2U;
	vkGetPhysicalDeviceSparseImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, properties);
	assert(count == 0U);
	peer.sparse_total = 2U;

	/* A present output pointer with zero capacity cannot become a native count-only query or device loss. */
	count = 0U;
	vkGetImageSparseMemoryRequirements(device, image, &count, guard);
	assert(count == 0U);
	assert(device->object.context->error == VK_SUCCESS);
	count = 0U;
	vkGetPhysicalDeviceSparseImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, guard);
	assert(count == 0U);
	assert(device->object.context->error == VK_SUCCESS);

	/* Succeeded: sparse outputs obey capacity and preserve the underlying context. */
	return;
}

/* Exercises real creation, native binding and queries before dependency-ordered destruction. */
static void
test_resources(
	struct VkDevice_T *device,
	struct VkPhysicalDevice_T *physical,
	void *guard)
{
	struct callback_state creator;
	struct callback_state destroyer;
	VkAllocationCallbacks create_callbacks;
	VkAllocationCallbacks destroy_callbacks;
	VkBufferCreateInfo buffer_info;
	VkBufferViewCreateInfo view_info;
	VkShaderModuleCreateInfo shader_info;
	VkSamplerCreateInfo sampler_info;
	VkFramebufferCreateInfo framebuffer_info;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkExtent2D granularity;
	VkBuffer buffer;
	VkBuffer concurrent_buffer;
	VkBufferView buffer_view;
	VkImage color;
	VkImage depth;
	VkImage sparse;
	VkImageView views[2];
	VkShaderModule shaders[2];
	VkSampler sampler;
	VkRenderPass pass;
	VkFramebuffer framebuffer;
	struct vulkan_object *memory;
	VkDeviceMemory memory_handle;
	VkResult status;
	uint64_t identity;
	uint32_t families[2] = { 3U, 7U };

	/* Compatible callbacks differ only in userdata, so final destruction must use the supplied policy. */
	memset(&creator, 0, sizeof(creator));
	memset(&destroyer, 0, sizeof(destroyer));
	callbacks_init(&create_callbacks, &creator);
	callbacks_init(&destroy_callbacks, &destroyer);
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = TEST_BUFFER_BYTES;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	buffer_info.queueFamilyIndexCount = 3U;
	buffer_info.pQueueFamilyIndices = guard;
	status = vkCreateBuffer(device, &buffer_info, &create_callbacks, &buffer);
	assert(status == VK_SUCCESS);

	/* The same create API must encode the active concurrent array without altering its indices. */
	buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
	buffer_info.queueFamilyIndexCount = 2U;
	buffer_info.pQueueFamilyIndices = families;
	status = vkCreateBuffer(device, &buffer_info, NULL, &concurrent_buffer);
	assert(status == VK_SUCCESS);

	/* Image creation supplies actual dependencies for both views and the later framebuffer. */
	color = create_image(device, VK_FORMAT_R8G8B8A8_UNORM, 0U, VK_SHARING_MODE_CONCURRENT, guard);
	depth = create_image(device, VK_FORMAT_D32_SFLOAT, 0U, VK_SHARING_MODE_EXCLUSIVE, guard);
	sparse = create_image(device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT, VK_SHARING_MODE_EXCLUSIVE, guard);
	/* Requirement outputs preserve large sizes, alignment and native memory-type indices. */
	memset(&requirements, 0, sizeof(requirements));
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	assert(requirements.size == UINT64_C(0x123450000));
	assert(requirements.alignment == 8192U);
	assert(requirements.memoryTypeBits == UINT32_C(0x80000021));
	memset(&requirements, 0, sizeof(requirements));
	vkGetImageMemoryRequirements(device, color, &requirements);
	assert(requirements.size == UINT64_C(0x123450000));
	assert(requirements.alignment == 8192U);
	assert(requirements.memoryTypeBits == UINT32_C(0x80000021));

	/* A failed native bind preserves the unbound resource and allows the ordinary retry. */
	memory = fake_memory(device);
	memory_handle = (VkDeviceMemory)(uintptr_t)vulkan_nondispatchable_handle(memory);
	identity = wire_id((uint64_t)(uintptr_t)buffer);
	peer.fail_opcode = 28U;
	peer.failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	status = vkBindBufferMemory(device, buffer, memory_handle, UINT64_C(0x100002000));
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(peer.objects[identity].bound_memory == 0U);
	status = vkBindBufferMemory(device, buffer, memory_handle, UINT64_C(0x100002000));
	assert(status == VK_SUCCESS);
	assert(peer.objects[identity].bound_memory == memory->wire_id);
	assert(peer.objects[identity].bound_offset == UINT64_C(0x100002000));
	status = vkBindImageMemory(device, color, memory_handle, UINT64_C(0x200004000));
	assert(status == VK_SUCCESS);
	identity = wire_id((uint64_t)(uintptr_t)color);
	assert(peer.objects[identity].bound_memory == memory->wire_id);
	assert(peer.objects[identity].bound_offset == UINT64_C(0x200004000));

	/* The depth attachment also binds ordinary memory before any view or framebuffer references it. */
	status = vkBindImageMemory(device, depth, memory_handle, UINT64_C(0x300006000));
	assert(status == VK_SUCCESS);

	/* Texel and image views are created only after their nonsparse storage has been bound. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
	view_info.buffer = buffer;
	view_info.format = VK_FORMAT_R32_UINT;
	view_info.offset = 4096U;
	view_info.range = 8192U;
	status = vkCreateBufferView(device, &view_info, NULL, &buffer_view);
	assert(status == VK_SUCCESS);

	views[0] = create_image_view(device, color, VK_FORMAT_R8G8B8A8_UNORM);
	views[1] = create_image_view(device, depth, VK_FORMAT_D32_SFLOAT);

	/* Native shaders retain every original module word rather than using artificial wire-only blobs. */
	memset(&shader_info, 0, sizeof(shader_info));
	shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = sizeof(vkdemo_vertex_shader);
	shader_info.pCode = vkdemo_vertex_shader;
	status = vkCreateShaderModule(device, &shader_info, NULL, &shaders[0]);
	assert(status == VK_SUCCESS);
	shader_info.codeSize = sizeof(vkdemo_fragment_shader);
	shader_info.pCode = vkdemo_fragment_shader;
	status = vkCreateShaderModule(device, &shader_info, NULL, &shaders[1]);
	assert(status == VK_SUCCESS);

	/* Distinct finite sampler values expose scalar order and floating-point representation errors. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_LINEAR;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	sampler_info.mipLodBias = 0.25F;
	sampler_info.maxAnisotropy = 1.0F;
	sampler_info.compareEnable = VK_TRUE;
	sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	sampler_info.minLod = 0.5F;
	sampler_info.maxLod = 4.0F;
	sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	status = vkCreateSampler(device, &sampler_info, NULL, &sampler);
	assert(status == VK_SUCCESS);

	/* The framebuffer references real native views and a metadata-owning render pass. */
	pass = create_render_pass(device, guard, &create_callbacks, &status);
	assert(status == VK_SUCCESS);
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = pass;
	framebuffer_info.attachmentCount = 2U;
	framebuffer_info.pAttachments = views;
	framebuffer_info.width = 128U;
	framebuffer_info.height = 64U;
	framebuffer_info.layers = 1U;
	status = vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer);
	assert(status == VK_SUCCESS);

	/* Linear subresource and render-pass geometry decode every independent native output field. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource.mipLevel = 1U;
	subresource.arrayLayer = 2U;
	memset(&layout, 0, sizeof(layout));
	vkGetImageSubresourceLayout(device, color, &subresource, &layout);
	assert(layout.offset == UINT64_C(0x100001000));
	assert(layout.size == UINT64_C(0x100002000));
	assert(layout.rowPitch == UINT64_C(0x100003000));
	assert(layout.arrayPitch == UINT64_C(0x100004000));
	assert(layout.depthPitch == UINT64_C(0x100005000));
	vkGetRenderAreaGranularity(device, pass, &granularity);
	assert(granularity.width == 8U);
	assert(granularity.height == 16U);
	test_sparse_queries(device, physical, sparse, guard);

	/* Destruction follows resource dependencies and preserves compatible callback userdata. */
	vkDestroyFramebuffer(device, framebuffer, NULL);
	vkDestroyRenderPass(device, pass, &destroy_callbacks);
	vkDestroySampler(device, sampler, NULL);
	vkDestroyShaderModule(device, shaders[1], NULL);
	vkDestroyShaderModule(device, shaders[0], NULL);
	vkDestroyImageView(device, views[1], NULL);
	vkDestroyImageView(device, views[0], NULL);
	vkDestroyImage(device, sparse, NULL);
	vkDestroyImage(device, depth, NULL);
	vkDestroyImage(device, color, NULL);
	vkDestroyBufferView(device, buffer_view, NULL);
	vkDestroyBuffer(device, concurrent_buffer, NULL);
	vkDestroyBuffer(device, buffer, &destroy_callbacks);
	memset(&peer.objects[memory->wire_id], 0, sizeof(peer.objects[0]));
	vulkan_object_free(memory);
	assert(creator.live == 0U);
	assert(creator.object_frees == 0U);
	assert(destroyer.object_frees == 5U);

	/* Succeeded: normal resource lifetime leaves no local or native object behind. */
	assert(device->object.first_child == NULL);
	assert_empty();
	return;
}

/* Injects failure into each independently allocated render-pass metadata owner. */
static void
test_render_failures(
	struct VkDevice_T *device,
	void *guard)
{
	struct callback_state state;
	VkAllocationCallbacks callbacks;
	VkRenderPass pass;
	VkResult status;
	unsigned index;
	unsigned calls;
	unsigned children;

	/* Object, attachment, color-subpass and depth-subpass allocations all precede native publication. */
	children = child_count(&device->object);
	for (index = 1U; index <= 4U; index++) {
		memset(&state, 0, sizeof(state));
		state.fail_object = index;
		callbacks_init(&callbacks, &state);
		calls = peer.calls[82];
		pass = create_render_pass(device, guard, &callbacks, &status);
		assert(status == VK_ERROR_OUT_OF_HOST_MEMORY);
		assert(pass == VK_NULL_HANDLE);
		assert(state.live == 0U);
		assert(peer.calls[82] == calls);
		assert(child_count(&device->object) == children);
	}

	/* A native failure after complete local preparation releases all four unpublished owners. */
	memset(&state, 0, sizeof(state));
	callbacks_init(&callbacks, &state);
	peer.fail_opcode = 82U;
	peer.failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	pass = create_render_pass(device, guard, &callbacks, &status);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(pass == VK_NULL_HANDLE);
	assert(state.live == 0U);
	assert(child_count(&device->object) == children);
	assert_empty();

	/* Succeeded: no partial metadata survives either local or native failure. */
	return;
}

/* Exercises failed creation and a foreign native handle without hiding namespace loss. */
static void
test_native_failures(
	void *guard)
{
	struct vulkan_context context;
	struct VkDevice_T device;
	struct callback_state state;
	VkAllocationCallbacks callbacks;
	VkBufferCreateInfo info;
	VkBuffer buffer;
	VkResult status;
	uint32_t calls;
	uint32_t index;

	/* A separate context lets terminal native inconsistency remain sticky without contaminating normal tests. */
	memset(&context, 0, sizeof(context));
	memset(&device, 0, sizeof(device));
	memset(&state, 0, sizeof(state));
	device.object.context = &context;
	device.object.wire_id = TEST_DEVICE_ID;
	callbacks_init(&callbacks, &state);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = TEST_BUFFER_BYTES;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.queueFamilyIndexCount = 3U;
	info.pQueueFamilyIndices = guard;
	peer.fail_opcode = 50U;
	peer.failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	buffer = (VkBuffer)(uintptr_t)UINT64_C(0xfeedbeef);
	status = vkCreateBuffer(&device, &info, &callbacks, &buffer);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(buffer == (VkBuffer)(uintptr_t)UINT64_C(0xfeedbeef));
	assert(context.error == VK_SUCCESS);
	assert(state.live == 0U);
	assert(device.object.first_child == NULL);
	assert_empty();

	/* A successful native create with a foreign echo leaves remote cleanup to terminal context destruction. */
	peer.foreign_echo = 1U;
	status = vkCreateBuffer(&device, &info, &callbacks, &buffer);
	assert(status == VK_ERROR_DEVICE_LOST);
	assert(buffer == (VkBuffer)(uintptr_t)UINT64_C(0xfeedbeef));
	assert(context.error == VK_ERROR_DEVICE_LOST);
	assert(state.live == 0U);
	assert(device.object.first_child == NULL);
	calls = peer.calls[50];
	status = vkCreateBuffer(&device, &info, &callbacks, &buffer);
	assert(status == VK_ERROR_DEVICE_LOST);
	assert(peer.calls[50] == calls);
	assert(state.live == 0U);

	/* Simulated renderer namespace close releases the unknown native result without inventing a local destroy. */
	for (index = 0U; index < TEST_OBJECTS; index++)
		memset(&peer.objects[index], 0, sizeof(peer.objects[index]));

	/* Succeeded: ordinary native errors allow retry, while a malformed successful result remains terminal. */
	assert_empty();
	return;
}

/* Checks native namespace and callback ownership after every complete scenario. */
static void
assert_empty(void)
{
	unsigned index;

	/* Local API cleanup must release every independently recorded native object. */
	for (index = 0U; index < TEST_OBJECTS; index++)
		assert(peer.objects[index].kind == 0U);

	/* Callback allocations cannot remain reachable after their resource owner has been consumed. */
	for (index = 0U; index < TEST_ALLOCATIONS; index++)
		assert(callback_allocations[index].address == NULL);

	/* Succeeded: neither namespace retains hidden resources. */
	return;
}
