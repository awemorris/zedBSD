/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Real client record encoding/decoding crosses the native transport and typed root/query implementation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include "userland/desktop/libvulkan/internal.h"
#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-device.h"
#include "drivers/gpu/bcm2711/vulkan-memory.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"
#include "drivers/gpu/bcm2711/vulkan-input.h"
#include "drivers/gpu/bcm2711/vulkan-layout.h"
#include "drivers/gpu/bcm2711/vulkan-descriptor.h"
#include "drivers/gpu/bcm2711/vulkan-target.h"
#include "drivers/gpu/bcm2711/vulkan-pipeline.h"
#include "userland/desktop/wayland/shaders.h"

/* One ordinary fixture reply blob retains its CPU owner across actual stream decoding. */
static struct bcm2711_buffer reply_buffer;

/* A single fixture descriptor is the native transport's only allowed reply resource. */
static struct bcm2711_render_resource reply_resource;

/* The reply region is naturally aligned for actual release-atomic decoder completion. */
static uint32_t reply_storage[2048];

/* The ordinary heap observation includes every actual root, registry and session arena allocation. */
static unsigned allocations;

/* A selected ordinary heap failure must unwind parent/domain ownership acquired by native publication. */
static unsigned fail_after;

/* Actual VA/page-table code consumes fixture RAM and an explicitly observed flush outcome. */
static uint32_t native_pages[1048576];

/* The page-table owner is fixture storage, not a separately allocated native backing run. */
static struct bcm2711_buffer page_buffer;

/* One imported allocation descriptor belongs to the fixture's exact open. */
static struct bcm2711_render_resource imported_resource;

/* Allocation observations distinguish declarations from first-export physical storage. */
static unsigned backing_allocations;

/* One failed native flush closes admission and exercises actual VA quarantine ownership. */
static unsigned fail_sync;

/* Fixture coherent storage provides valid host spans without simulating actual physical placement/cache. */
static uint8_t backing_storage[16384];

static void pipeline_test(struct bcm2711_vulkan_session *session);
static void target_test(struct bcm2711_vulkan_session *session);
static void descriptor_test(struct bcm2711_vulkan_session *session);
static void encode_image_write(struct vulkan_writer *writer, uint64_t set, uint64_t sampler, uint64_t view);
static void pool_test(struct bcm2711_vulkan_session *session);
static int sets_allocate(struct bcm2711_vulkan_session *session, uint64_t pool, uint64_t layout, uint64_t first);
static void layout_test(struct bcm2711_vulkan_session *session);
static void input_test(struct bcm2711_vulkan_session *session);
static int input_created(struct bcm2711_vulkan_session *session, struct vulkan_writer *writer, uint64_t identity);
static void resource_test(struct bcm2711_vulkan_session *session);
static void memory_test(struct bcm2711_vulkan_session *session, struct bcm2711_render_session *render);
static int memory_allocate(struct bcm2711_vulkan_session *session, uint64_t identity, uint64_t bytes, uint32_t extension, uint32_t argument);
static void memory_free(struct bcm2711_vulkan_session *session, uint64_t identity);
static int dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void begin(struct vulkan_writer *writer, void *storage, size_t capacity, uint32_t opcode, uint32_t requested);
static int execute(struct bcm2711_vulkan_session *session, struct vulkan_writer *writer, struct vulkan_reader *reader);
static void encode_instance(struct vulkan_writer *writer, uint64_t identity, uint32_t version);
static void encode_device(struct vulkan_writer *writer, uint64_t physical, uint64_t identity, uint32_t family, VkPhysicalDeviceFeatures *features);
static void encode_queue(struct vulkan_writer *writer, uint64_t device, uint64_t identity, uint32_t timeline);
static void destroy(struct bcm2711_vulkan_session *session, enum gpu_op opcode, uint64_t identity);

/*
 * Supplies ordinary host allocation while observing native descriptor and arena unwind.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *pointer;

	/* A selected ordinary allocation can refuse a root or registry before publication. */
	if (fail_after != 0) {
		fail_after--;
		if (fail_after == 0)
			return NULL;
	}

	/* The fixture counts only actual storage returned to native source. */
	pointer = calloc(count, bytes);
	if (pointer != NULL)
		allocations++;

	/* Succeeded: the exact host allocation outcome reaches the ordinary native allocator call. */
	return pointer;
}

/*
 * Supplies ordinary private-stream allocation with the same observable heap ownership.
 */
void *
kern_malloc(
	size_t bytes)
{
	void *pointer;

	/* The fixture uses one counted ordinary allocation without a production-only switch. */
	pointer = kern_calloc(1, bytes);
	if (pointer == NULL)
		return NULL;

	/* Succeeded: external native stream copies would acquire observable storage through the same allocator. */
	return pointer;
}

/*
 * Releases actual native logical storage after its final owner retires.
 */
void
kern_free(
	void *pointer)
{
	/* Partial construction owns no allocation when its pointer is absent. */
	if (pointer == NULL)
		return;
	assert(allocations != 0);
	allocations--;
	free(pointer);

	/* Succeeded: the fixture no longer counts this final retired allocation. */
	return;
}

/*
 * Finds the fixture's sole actual native reply descriptor in its exact session namespace.
 */
struct bcm2711_render_resource *
bcm2711_render_find(
	struct bcm2711_render_session *session,
	uint32_t identity)
{
	/* Missing or foreign resources cannot become reply backing. */
	if (session != reply_resource.owner)
		return NULL;
	if (identity == 2 && imported_resource.view != NULL)
		return &imported_resource;
	if (identity != 1)
		return NULL;

	/* Succeeded: this borrowed descriptor remains fixture-owned through native stream execution. */
	return &reply_resource;
}

/*
 * Retains the ordinary reply buffer independently of its resource identity.
 */
void
bcm2711_buffer_retain(
	struct bcm2711_buffer *buffer)
{
	/* This fixture exposes only one live CPU buffer, never physical GPU ownership. */
	assert(buffer != NULL && buffer->references != 0);
	buffer->references++;
}

/*
 * Releases the actual transport's independent CPU reply hold.
 */
void
bcm2711_buffer_release(
	struct bcm2711_buffer *buffer)
{
	/* The native release must never receive NULL or consume the fixture's descriptor reference. */
	assert(buffer != NULL && buffer->references != 0);
	if (buffer == &reply_buffer) {
		assert(buffer->references > 1);
		buffer->references--;
		return;
	}

	/* Independent GPU views and returned BLOB source holds retire before their final fixture RAM owner. */
	buffer->references--;
	if (buffer->references == 0)
		kern_free(buffer);
}

/*
 * Supplies a bounded coherent fixture run while observing the actual late placement requirements.
 */
int
bcm2711_buffer_create_uncached(
	uint64_t bytes,
	uint64_t limit,
	size_t alignment,
	struct bcm2711_buffer **result)
{
	struct bcm2711_buffer *buffer;

	/* The fixture does not model physical allocation or cache attributes; separate NC/VM source tests cover those owners. */
	*result = NULL;
	assert(limit <= 0x3fffffffU && alignment >= 4096 && alignment <= 0x100000);
	buffer = kern_calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;
	buffer->bytes = bytes;
	buffer->memory.size = (bytes + 4095U) & ~4095ULL;
	buffer->memory.paddr = 0x100000;
	buffer->address = backing_storage;
	buffer->uncached = true;
	buffer->references = 1;
	backing_allocations++;
	*result = buffer;

	/* Succeeded: actual native VA ownership will retain this ordinary fixture source run. */
	return 0;
}

/*
 * Supplies an explicit native flush outcome without emulating actual hardware retirement.
 */
int
bcm2711_v3d_hardware_pages_sync(
	struct bcm2711_v3d *engine)
{
	/* Native flush refusal closes subsequent admission; actual VA source must retain its mapped storage. */
	if (fail_sync != 0) {
		fail_sync--;
		engine->hardware.ready = false;
		return EIO;
	}

	/* Succeeded: the software fixture permits the actual VA owner to finish its selected operation. */
	return 0;
}

/*
 * Exercises real discovery/device records, queue ownership and explicit unsupported/fault outcomes.
 */
int
main(
	void)
{
	struct bcm2711_v3d engine;
	struct bcm2711_render_device controller;
	struct bcm2711_render_session render;
	struct bcm2711_vulkan_session *session;
	struct bcm2711_v3d_view view;
	struct bcm2711_vulkan_object *queue;
	struct bcm2711_vulkan_object *found;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceMemoryProperties memory;
	VkQueueFamilyProperties family;
	VkFormatProperties format;
	VkImageFormatProperties image;
	uint8_t wire[4096];
	uint64_t identity;
	uint64_t count;
	uint32_t index;
	unsigned baseline;
	int error;

	/* Native controller state is ready without simulating a physical MMIO queue or scheduler. */
	memset(&engine, 0, sizeof(engine));
	memset(&controller, 0, sizeof(controller));
	memset(&render, 0, sizeof(render));
	memset(&view, 0, sizeof(view));
	engine.hardware.ready = true;
	engine.hardware.physical_bits = 32;
	page_buffer.address = native_pages;
	engine.hardware.pages = &page_buffer;
	spin_init(&engine.hardware.guard, LOCK_RANK_DEVICE, "vulkan-test");
	controller.space.native = &engine;
	render.device = &controller;
	view.buffer = &reply_buffer;
	reply_buffer.address = reply_storage;
	reply_buffer.bytes = sizeof(reply_storage);
	reply_buffer.references = 1;
	reply_resource.owner = &render;
	reply_resource.blob = true;
	reply_resource.view = &view;
	error = bcm2711_vulkan_session_open(&render, &session);
	assert(error == 0);

	/* The actual client instance record creates an independent typed native namespace. */
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_INSTANCE, 1);
	encode_instance(&writer, 10, 0x00401000U);
	error = execute(session, &writer, &reader);
	assert(error == 0);
	assert(vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(vulkan_read_u64(&reader) == 1);
	identity = vulkan_read_u64(&reader);
	assert(identity == 10 && reader.error == VK_SUCCESS);

	/* Physical enumeration first asks the count, then publishes the exact guest-chosen native output identity. */
	begin(&writer, wire, sizeof(wire), GPU_OP_ENUMERATE_PHYSICAL_DEVICES, 1);
	vulkan_write_u64(&writer, 10);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(vulkan_read_u64(&reader) == 1 && vulkan_read_u32(&reader) == 1 && vulkan_read_u64(&reader) == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_ENUMERATE_PHYSICAL_DEVICES, 1);
	vulkan_write_u64(&writer, 10);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 20);
	error = execute(session, &writer, &reader);
	assert(error == 0 && session->object_count == 2);

	/* Actual generated client decoding checks the complete property/feature/memory snapshot. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkPhysicalDeviceProperties(&reader, &properties);
	assert(reader.error == VK_SUCCESS && properties.apiVersion == 0x00401000U);
	assert(properties.vendorID == 0x14e4 && properties.deviceID == 0x402);
	assert(properties.limits.maxFramebufferWidth == 4096 && properties.limits.maxColorAttachments == 1);
	assert(properties.limits.framebufferColorSampleCounts == VK_SAMPLE_COUNT_1_BIT);
	assert(properties.limits.maxPushConstantsSize == 128 && properties.limits.timestampComputeAndGraphics == VK_FALSE);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_FEATURES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkPhysicalDeviceFeatures(&reader, &features);
	assert(reader.error == VK_SUCCESS);
	for (index = 0; index < sizeof(features); index++) {
		assert(((uint8_t *)&features)[index] == 0);
	}

	/* Native memory type zero requires actual Normal NC allocations, without a HOST_CACHED claim. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, VK_MAX_MEMORY_TYPES);
	vulkan_write_u64(&writer, VK_MAX_MEMORY_HEAPS);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkPhysicalDeviceMemoryProperties(&reader, &memory);
	assert(reader.error == VK_SUCCESS && memory.memoryTypeCount == 1 && memory.memoryHeapCount == 1);
	assert(memory.memoryTypes[0].propertyFlags == (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	assert(memory.memoryHeaps[0].size == (256U << 20));

	/* Count-only and array family queries describe exactly one graphics/transfer queue and no timestamps. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1 && vulkan_read_u32(&reader) == 1 && vulkan_read_u64(&reader) == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1 && vulkan_read_u32(&reader) == 1 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkQueueFamilyProperties(&reader, &family);
	assert(reader.error == VK_SUCCESS && family.queueCount == 1 && family.timestampValidBits == 0);
	assert(family.queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT));

	/* Supported native colour and unsupported depth formats remain distinct complete query results. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u32(&writer, VK_FORMAT_B8G8R8A8_UNORM);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkFormatProperties(&reader, &format);
	assert(reader.error == VK_SUCCESS && (format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES, 1);
	vulkan_write_u64(&writer, 20);
	vulkan_write_u32(&writer, VK_FORMAT_D32_SFLOAT);
	vulkan_write_u32(&writer, VK_IMAGE_TYPE_2D);
	vulkan_write_u32(&writer, VK_IMAGE_TILING_OPTIMAL);
	vulkan_write_u32(&writer, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkImageFormatProperties(&reader, &image);
	assert(reader.error == VK_SUCCESS && image.maxExtent.width == 0 && image.sampleCounts == 0);

	/* An actual optional feature request fails explicitly without publishing a logical device. */
	features.geometryShader = VK_TRUE;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DEVICE, 1);
	encode_device(&writer, 20, 30, 0, &features);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
	found = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, 30);
	assert(found == NULL);

	/* A normal Keiland queue request creates a retained native device/queue graph and its exact timeline. */
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DEVICE, 1);
	encode_device(&writer, 20, 30, 0, NULL);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 30, 40, 7);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == 40);
	assert(render.timelines == (UINT64_C(1) << 7));
	queue = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUEUE, 40);
	assert(queue != NULL);
	memory_test(session, &render);
	resource_test(session);
	input_test(session);
	layout_test(session);
	pool_test(session);
	descriptor_test(session);
	baseline = allocations;

	/* Repeating the exact native lookup neither reallocates a root nor changes its completion domain. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 30, 40, 7);
	error = execute(session, &writer, &reader);
	assert(error == 0 && allocations == baseline);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 30, 41, 8);
	error = execute(session, &writer, &reader);
	assert(error == EEXIST && allocations == baseline && render.timelines == (UINT64_C(1) << 7));

	/* Native busy/fault state cannot be hidden by an unconditional idle reply. */
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_WAIT_IDLE, 1);
	vulkan_write_u64(&writer, 40);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	engine.hardware.job_busy = true;
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_WAIT_IDLE, 1);
	vulkan_write_u64(&writer, 40);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_DEVICE_LOST);
	engine.hardware.job_busy = false;

	/* Removing a device's wire identity preserves a prepared queue reference and its original domain. */
	error = bcm2711_vulkan_object_retain(queue);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_DEVICE, 30);
	found = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUEUE, 40);
	assert(found == NULL && queue->references == 1 && render.timelines == (UINT64_C(1) << 7));
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DEVICE, 1);
	encode_device(&writer, 20, 31, 0, NULL);
	error = execute(session, &writer, &reader);
	assert(error == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 31, 42, 7);
	error = execute(session, &writer, &reader);
	assert(error == EEXIST);
	error = bcm2711_vulkan_object_release(queue);
	assert(error == 0 && render.timelines == 0);

	/* An old common callback slot prevents timeline recycling even after the old queue's last owner retired. */
	controller.worker.requests[0].session = &render;
	controller.worker.requests[0].timeline = 7;
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 31, 42, 7);
	error = execute(session, &writer, &reader);
	assert(error == EAGAIN && render.timelines == 0);
	memset(&controller.worker.requests[0], 0, sizeof(controller.worker.requests[0]));

	/* Failed registry allocation releases both the acquired parent edge and unpublished timeline reservation. */
	baseline = allocations;
	fail_after = 2;
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 31, 42, 7);
	error = execute(session, &writer, &reader);
	assert(error == ENOMEM && allocations == baseline && render.timelines == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_DEVICE_QUEUE2, 1);
	encode_queue(&writer, 31, 42, 7);
	error = execute(session, &writer, &reader);
	assert(error == 0 && render.timelines == (UINT64_C(1) << 7));

	/* Absent physical identities and unsupported queue families are never replaced by an arbitrary available device. */
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DEVICE, 1);
	encode_device(&writer, 999, 99, 0, NULL);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL);
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DEVICE, 1);
	encode_device(&writer, 20, 99, 1, NULL);
	error = execute(session, &writer, &reader);
	assert(error == ENOTSUP);

	/* Final implicit root cleanup releases every native graph edge, session arena and completion domain. */
	destroy(session, GPU_OP_DESTROY_DEVICE, 31);
	destroy(session, GPU_OP_DESTROY_INSTANCE, 10);
	error = bcm2711_vulkan_session_close(&session);
	assert(error == 0 && session == NULL && allocations == 0 && render.timelines == 0 && reply_buffer.references == 1);
	count = reply_buffer.references;
	assert(count == 1);
	puts("WS141 Vulkan actual client records/native discovery/device/domain ownership: PASS");

	/* Succeeded: actual native software semantics are verified without claiming physical GPU or scheduler execution. */
	return 0;
}

/* Routes the actual immutable command through native roots, then native physical queries. */
static int
dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	int handled;
	int error;

	/* Device root routing must either consume one exact opcode or leave it unchanged for another typed module. */
	error = bcm2711_vulkan_device_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Actual physical records use the independently implemented native query table. */
	error = bcm2711_vulkan_query_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Allocation commands retain native views and real typed device ownership. */
	error = bcm2711_vulkan_memory_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Native storage requirements and retained memory binding consume their actual typed commands. */
	error = bcm2711_vulkan_resource_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Immutable native image views, samplers and owned SPIR-V modules retain their exact typed parents. */
	error = bcm2711_vulkan_input_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Canonical descriptor and pipeline interfaces retain immutable dependency graphs independently of public IDs. */
	error = bcm2711_vulkan_layout_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Actual pool lifecycle and complete set batches acquire independently retained native graph ownership. */
	error = bcm2711_vulkan_descriptor_pool_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;
	error = bcm2711_vulkan_descriptor_sets_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	/* Ordered mutable bindings acquire independent draw snapshots before later updates can retire their original resources. */
	if (handled != 0)
		return 0;
	error = bcm2711_vulkan_descriptor_update_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	/* Immutable target owners keep attachment semantics and native storage through later command preparation. */
	if (handled != 0)
		return 0;
	error = bcm2711_vulkan_target_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled == 0)
		return ENOTSUP;

	/* Succeeded: one real typed native module consumed the complete selected command. */
	return 0;
}

/* Begins a real client-encoded command with an actual session reply selection. */
static void
begin(
	struct vulkan_writer *writer,
	void *storage,
	size_t capacity,
	uint32_t opcode,
	uint32_t requested)
{
	uint8_t header[8];

	/* Fixed fixture storage uses actual primitives without invoking an unrelated local Vulkan object allocator. */
	memset(writer, 0, sizeof(*writer));
	writer->data = storage;
	writer->capacity = capacity;

	/* The actual native context negotiates opaque allocation capabilities before encoding public external declarations. */
	writer->external_memory_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	vulkan_command_begin(writer, opcode);
	assert(requested == 1 && writer->bytes == sizeof(header));
	memcpy(header, writer->data, sizeof(header));
	writer->bytes = 0;
	vulkan_write_u32(writer, GPU_OP_SET_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, sizeof(reply_storage));

	/* The actual ordinary client header requests an echoed opcode even for void Vulkan commands. */
	memcpy(writer->data + writer->bytes, header, sizeof(header));
	writer->bytes += sizeof(header);
}

/* Executes through actual native transport and makes a genuine client decoder cursor over the returned payload. */
static int
execute(
	struct bcm2711_vulkan_session *session,
	struct vulkan_writer *writer,
	struct vulkan_reader *reader)
{
	int error;

	/* The real completion wrapper follows typed commands and contributes the same bounded trailing wire context as libvulkan. */
	vulkan_write_u32(writer, GPU_OP_SEEK_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, sizeof(reply_storage) - 20);
	vulkan_write_u32(writer, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);
	assert(writer->error == VK_SUCCESS);
	memset(reply_storage, 0xa5, sizeof(reply_storage));
	error = bcm2711_vulkan_stream_execute(session, writer->data, (uint32_t)writer->bytes, dispatch);
	assert(reply_buffer.references == 1);

	/* A refused typed command never fabricates a successful final completion trailer. */
	if (error != 0)
		return error;
	assert(reply_storage[2047] == 0x00401000U);
	memset(reader, 0, sizeof(*reader));
	reader->data = (uint8_t *)reply_storage + 4;
	reader->bytes = sizeof(reply_storage) - 24;

	/* Succeeded: the caller can decode the actual command payload after its echoed opcode. */
	return 0;
}

/* Encodes the real instance record with the private transport's native version. */
static void
encode_instance(
	struct vulkan_writer *writer,
	uint64_t identity,
	uint32_t version)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo info;

	/* The client strips native layers/extensions while retaining actual application metadata. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.apiVersion = version;
	application.pApplicationName = "WS141 fixture";
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	vulkan_write_u64(writer, 1);
	vulkan_encode_VkInstanceCreateInfo(writer, &info);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, identity);
}

/* Encodes the actual generated logical-device/queue creation record used by Keiland. */
static void
encode_device(
	struct vulkan_writer *writer,
	uint64_t physical,
	uint64_t identity,
	uint32_t family,
	VkPhysicalDeviceFeatures *features)
{
	VkDeviceCreateInfo info;
	VkDeviceQueueCreateInfo queue;
	float priority;
	uint32_t bits;

	/* A real float priority is encoded by its actual client codec without a native kernel FP calculation. */
	bits = 0x3f800000U;
	memcpy(&priority, &bits, sizeof(priority));
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue;
	info.pEnabledFeatures = features;
	vulkan_write_u64(writer, physical);
	vulkan_write_u64(writer, 1);
	vulkan_encode_VkDeviceCreateInfo(writer, &info);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, identity);
}

/* Encodes the real private GetDeviceQueue2 chained timeline record without a mock ABI. */
static void
encode_queue(
	struct vulkan_writer *writer,
	uint64_t device,
	uint64_t identity,
	uint32_t timeline)
{
	/* These fields match the pinned device_queue_remote wire emitted by the actual library. */
	vulkan_write_u64(writer, device);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 1000384005U);
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, timeline);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, identity);
}

/* Destroys an actual typed native root or resource through its ordinary echoed-opcode command. */
static void
destroy(
	struct bcm2711_vulkan_session *session,
	enum gpu_op opcode,
	uint64_t identity)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[128];
	int error;

	/* Native allocation callbacks remain absent and implicit child identities retire inside the actual command. */
	begin(&writer, wire, sizeof(wire), opcode, 1);
	if (opcode != GPU_OP_DESTROY_INSTANCE && opcode != GPU_OP_DESTROY_DEVICE)
		vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, identity);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0);
}

/* Exercises late placement, independent BLOB/import references and failed-flush quarantine through actual native VA source. */
static void
memory_test(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_render_session *render)
{
	struct gpu_blob_create request;
	struct gpu_placement placement;
	struct bcm2711_buffer *first;
	struct bcm2711_buffer *second;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_v3d_view *view;
	unsigned baseline;
	int error;

	/* A declared allocation reserves its budget and device edge without allocating physical backing yet. */
	baseline = allocations;
	error = memory_allocate(session, 50, 8192, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, 0x200);
	assert(error == VK_SUCCESS && backing_allocations == 0 && render->device->vulkan_memory_bytes == 8192);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, 50);
	assert(object != NULL);
	memory = object->payload;
	assert(memory->view == NULL);

	/* The first placed BLOB must use the actual alignment and full inclusive DMA ceiling before creating a native view. */
	memset(&request, 0, sizeof(request));
	memset(&placement, 0, sizeof(placement));
	request.blob_id = 50;
	request.bytes = 8192;
	request.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE;
	placement.flags = GPU_PLACEMENT_COHERENT | GPU_PLACEMENT_CONTIGUOUS;
	placement.alignment = 65536;
	placement.max_dma_address = 0x1fffff;
	error = bcm2711_vulkan_memory_blob(session, &request, &placement, &first);
	assert(error == 0 && first != NULL && backing_allocations == 1 && first->references == 2);
	assert(memory->view != NULL && memory->view->references == 1);

	/* Repeated aliases retain one immutable backing allocation and reject conditions it cannot actually satisfy. */
	error = bcm2711_vulkan_memory_blob(session, &request, &placement, &second);
	assert(error == 0 && second == first && backing_allocations == 1 && first->references == 3);
	bcm2711_buffer_release(second);
	placement.max_dma_address = 0x100fff;
	error = bcm2711_vulkan_memory_blob(session, &request, &placement, &second);
	assert(error == ENOTSUP && second == NULL && first->references == 2);
	placement.max_dma_address = 0x1fffff;
	placement.alignment = 3;
	error = bcm2711_vulkan_memory_blob(session, &request, &placement, &second);
	assert(error == EINVAL && second == NULL);
	placement.alignment = 65536;

	/* One independent resource view can survive VkMemory removal and supply a coherent import into another typed declaration. */
	error = bcm2711_v3d_memory_map(&render->device->space, first, &view);
	assert(error == 0 && first->references == 3);
	imported_resource.owner = render;
	imported_resource.blob = true;
	imported_resource.shareable = true;
	imported_resource.view = view;
	error = memory_allocate(session, 51, 8192, 1000384002U, 2);
	assert(error == VK_SUCCESS && view->references == 2 && render->device->vulkan_memory_bytes == 16384);
	memory_free(session, 50);
	assert(first->references == 2 && render->device->vulkan_memory_bytes == 8192);
	bcm2711_buffer_release(first);

	/* The imported allocation independently owns its view after the original resource descriptor retires. */
	error = bcm2711_v3d_memory_release(&render->device->space, view);
	assert(error == 0 && view->references == 1);
	imported_resource.view = NULL;
	memory_free(session, 51);
	assert(render->device->space.views == NULL && render->device->vulkan_memory_bytes == 0 && allocations == baseline);

	/* Aggregate declared heap exhaustion has an explicit Vulkan failure and no new backing allocation. */
	error = memory_allocate(session, 52, 256ULL << 20, 0, 0);
	assert(error == VK_SUCCESS);
	error = memory_allocate(session, 53, 4096, 0, 0);
	assert(error == (int)VK_ERROR_OUT_OF_DEVICE_MEMORY && backing_allocations == 1);
	memory_free(session, 52);

	/* Both ordinary payload and registry allocation refusal retire every budget charge and acquired device dependency. */
	fail_after = 1;
	error = memory_allocate(session, 54, 4096, 0, 0);
	assert(error == (int)VK_ERROR_OUT_OF_HOST_MEMORY && allocations == baseline);
	fail_after = 2;
	error = memory_allocate(session, 54, 4096, 0, 0);
	assert(error == (int)VK_ERROR_OUT_OF_HOST_MEMORY && allocations == baseline && render->device->vulkan_memory_bytes == 0);

	/* Failed actual VA publication retains the backing in quarantine after logical memory identity retirement. */
	error = memory_allocate(session, 55, 8192, 0, 0);
	assert(error == VK_SUCCESS);
	request.blob_id = 55;
	request.flags = GPU_BLOB_MAPPABLE;
	fail_sync = 1;
	error = bcm2711_vulkan_memory_blob(session, &request, &placement, &first);
	assert(error == EIO && first == NULL && backing_allocations == 2);
	view = render->device->space.views;
	assert(view != NULL && view->quarantined && view->references == 0 && view->buffer->references == 1);
	memory_free(session, 55);
	assert(render->device->vulkan_memory_bytes == 0 && allocations == baseline + 2);

	/* The fixture explicitly supplies a later checked-reset boundary before actual quarantine recovery can release RAM. */
	render->device->space.native->hardware.ready = true;
	error = bcm2711_v3d_memory_recover(&render->device->space);
	assert(error == 0 && render->device->space.views == NULL && allocations == baseline);
	puts("WS141 Vulkan lazy memory/placed BLOB/import/budget/actual VA quarantine: PASS");
}

/* Encodes the actual client's allocation chain and decodes its full native result identity. */
static int
memory_allocate(
	struct bcm2711_vulkan_session *session,
	uint64_t identity,
	uint64_t bytes,
	uint32_t extension,
	uint32_t argument)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[256];
	uint32_t status;
	uint64_t returned;
	int error;

	/* The exact allocation wire follows memory.c, using the actual ordinary header and client primitives. */
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	if (extension == 0) {
		vulkan_write_u64(&writer, 0);
	} else {
		vulkan_write_u64(&writer, 1);
		vulkan_write_u32(&writer, extension);
		vulkan_write_u64(&writer, 0);
		vulkan_write_u32(&writer, argument);
	}

	/* Complete generated values are protocol identities, not host application pointers. */
	vulkan_write_u64(&writer, bytes);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, identity);
	error = execute(session, &writer, &reader);
	assert(error == 0);
	status = vulkan_read_u32(&reader);
	assert(vulkan_read_u64(&reader) == 1);
	returned = vulkan_read_u64(&reader);
	if (status == VK_SUCCESS)
		assert(returned == identity);
	else
		assert(returned == 0);

	/* Succeeded: the caller sees the real structured Vulkan allocation outcome. */
	return (int)status;
}

/* Sends the actual ordinary client void-command header and verifies echoed-opcode completion through native transport. */
static void
memory_free(
	struct bcm2711_vulkan_session *session,
	uint64_t identity)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[256];
	int error;

	/* FreeMemory carries no parameter reply, while its ordinary client header still requires an opcode echo. */
	begin(&writer, wire, sizeof(wire), GPU_OP_FREE_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, identity);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && reply_storage[0] == GPU_OP_FREE_MEMORY);
}

/* Exercises actual buffer/image codecs, immutable raster requirements and independently retained bindings through native source. */
static void
resource_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct bcm2711_vulkan_object *buffer;
	struct bcm2711_vulkan_resource *description;
	struct bcm2711_v3d_view *view;
	struct bcm2711_buffer *blob;
	struct gpu_blob_create request;
	VkBufferCreateInfo buffer_info;
	VkImageCreateInfo image_info;
	VkExternalMemoryImageCreateInfo external_image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	uint8_t wire[1024];
	void *cpu;
	uint32_t address;
	unsigned baseline;
	int error;

	/* The real buffer record creates a 65-byte public extent with a 128-byte native allocation requirement. */
	baseline = allocations;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = 65;
	buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_BUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferCreateInfo(&writer, &buffer_info);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 60);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == 60);
	buffer = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, 60);
	assert(buffer != NULL);
	description = buffer->payload;
	assert(description->bytes == 65 && description->required_bytes == 128 && description->memory == NULL);

	/* The actual client requirement decoder receives exact size/alignment/native coherent type bit. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 60);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkMemoryRequirements(&reader, &requirements);
	assert(reader.error == VK_SUCCESS && requirements.size == 128 && requirements.alignment == 64 && requirements.memoryTypeBits == 1);

	/* A linear non-square image reports one exact colour subresource, with native 64-byte row alignment. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	memset(&external_image, 0, sizeof(external_image));
	external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	image_info.pNext = &external_image;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	image_info.extent.width = 17;
	image_info.extent.height = 3;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_LINEAR;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, &image_info);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 61);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 61);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageSubresource(&writer, &subresource);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1);
	vulkan_decode_VkSubresourceLayout(&reader, &layout);
	assert(reader.error == VK_SUCCESS && layout.offset == 0 && layout.rowPitch == 128 && layout.size == 384);

	/* Coherent allocation backing is created before binding through the actual native memory/BLOB path. */
	error = memory_allocate(session, 62, 8192, 0, 0);
	assert(error == VK_SUCCESS);
	memset(&request, 0, sizeof(request));
	request.blob_id = 62;
	request.bytes = 8192;
	request.flags = GPU_BLOB_MAPPABLE;
	error = bcm2711_vulkan_memory_blob(session, &request, NULL, &blob);
	assert(error == 0);

	/* A misaligned bind refuses the operation without acquiring an allocation or changing the resource. */
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_BUFFER_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 60);
	vulkan_write_u64(&writer, 62);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && description->memory == NULL);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_BUFFER_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 60);
	vulkan_write_u64(&writer, 62);
	vulkan_write_u64(&writer, 64);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && description->offset == 64);

	/* The binding retains the typed allocation after its public memory identity and separate BLOB hold retire. */
	memory_free(session, 62);
	bcm2711_buffer_release(blob);
	error = bcm2711_vulkan_resource_backing(description, 1, 64, &view, &address, &cpu);
	assert(error == 0 && address == view->address + 65 && cpu == backing_storage + 65);
	error = bcm2711_vulkan_resource_backing(description, 65, 1, &view, &address, &cpu);
	assert(error == EINVAL && view == NULL && address == 0 && cpu == NULL);

	/* A prepared native owner survives registry destruction with its exact resource and memory binding intact. */
	error = bcm2711_vulkan_object_retain(buffer);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_BUFFER, 60);
	assert(buffer->references == 1 && session->render->device->vulkan_memory_bytes == 8192);
	error = bcm2711_vulkan_resource_backing(description, 0, 65, &view, &address, &cpu);
	assert(error == 0 && cpu == backing_storage + 64);
	error = bcm2711_vulkan_object_release(buffer);
	assert(error == 0 && session->render->device->vulkan_memory_bytes == 0);
	destroy(session, GPU_OP_DESTROY_IMAGE, 61);
	assert(allocations == baseline && session->render->device->space.views == NULL);
	puts("WS141 Vulkan actual buffer/image layout/requirements/binding/prepared owner: PASS");
}

/* Exercises actual colour-view/sampler/module records and their immutable ownership through dependent retirement. */
static void
input_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct bcm2711_vulkan_object *image;
	struct bcm2711_vulkan_object *view;
	struct bcm2711_vulkan_object *shader;
	struct bcm2711_vulkan_image_view *description;
	struct bcm2711_vulkan_module *module;
	struct vulkan_object client_image;
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkSamplerCreateInfo sampler_info;
	VkShaderModuleCreateInfo shader_info;
	uint8_t wire[4096];
	unsigned baseline;
	int error;

	/* A small actual colour image supplies the independently retained view parent. */
	baseline = allocations;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	image_info.extent.width = 16;
	image_info.extent.height = 8;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, &image_info);
	error = input_created(session, &writer, 70);
	assert(error == VK_SUCCESS);
	image = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, 70);
	assert(image != NULL && image->references == 1);

	/* Failed view registry publication must unwind the acquired image/device edges after ordinary payload allocation. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	memset(&client_image, 0, sizeof(client_image));
	client_image.wire_id = 70;
	view_info.image = (VkImage)(uintptr_t)&client_image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = image_info.format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	fail_after = 2;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE_VIEW, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageViewCreateInfo(&writer, &view_info);
	error = input_created(session, &writer, 71);
	assert(error == (int)VK_ERROR_OUT_OF_HOST_MEMORY && image->references == 1);
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE_VIEW, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageViewCreateInfo(&writer, &view_info);
	error = input_created(session, &writer, 71);
	assert(error == VK_SUCCESS && image->references == 2);
	view = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, 71);
	assert(view != NULL);
	description = view->payload;
	assert(description->owner.parent == image && description->format == image_info.format);

	/* A prepared view reference survives both public image and public view identity retirement. */
	error = bcm2711_vulkan_object_retain(view);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_IMAGE, 70);
	destroy(session, GPU_OP_DESTROY_IMAGE_VIEW, 71);
	assert(view->references == 1 && image->references == 1 && !image->published);
	error = bcm2711_vulkan_object_release(view);
	assert(error == 0 && allocations == baseline);

	/* Exact Keiland nearest and glass-linear sampler records create independent immutable native inputs. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SAMPLER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSamplerCreateInfo(&writer, &sampler_info);
	error = input_created(session, &writer, 72);
	assert(error == VK_SUCCESS);
	sampler_info.magFilter = VK_FILTER_LINEAR;
	sampler_info.minFilter = VK_FILTER_LINEAR;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SAMPLER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSamplerCreateInfo(&writer, &sampler_info);
	error = input_created(session, &writer, 73);
	assert(error == VK_SUCCESS);
	destroy(session, GPU_OP_DESTROY_SAMPLER, 72);
	destroy(session, GPU_OP_DESTROY_SAMPLER, 73);
	assert(allocations == baseline);

	/* The actual Keiland quad module is copied before the arena and original command storage are reused. */
	memset(&shader_info, 0, sizeof(shader_info));
	shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = sizeof(kwl_quad_vert);
	shader_info.pCode = kwl_quad_vert;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SHADER_MODULE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkShaderModuleCreateInfo(&writer, &shader_info);
	error = input_created(session, &writer, 74);
	assert(error == VK_SUCCESS);
	shader = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SHADER_MODULE, 74);
	assert(shader != NULL);
	module = shader->payload;
	assert(module->word_count == sizeof(kwl_quad_vert) / sizeof(uint32_t));
	memset(session->arena.base, 0xa5, session->arena.size);
	memset(wire, 0x5a, sizeof(wire));
	assert(memcmp(module->words, kwl_quad_vert, sizeof(kwl_quad_vert)) == 0);
	error = bcm2711_vulkan_object_retain(shader);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_SHADER_MODULE, 74);
	assert(shader->references == 1 && memcmp(module->words, kwl_quad_vert, sizeof(kwl_quad_vert)) == 0);
	error = bcm2711_vulkan_object_release(shader);
	assert(error == 0 && allocations == baseline);

	/* An absent source array cannot masquerade as the same nonzero declared SPIR-V extent. */
	shader_info.pCode = NULL;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SHADER_MODULE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkShaderModuleCreateInfo(&writer, &shader_info);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 75);
	error = execute(session, &writer, &reader);
	assert(error == ENOTSUP && allocations == baseline);
	puts("WS141 Vulkan actual colour view/nearest-linear sampler/copied Keiland SPIR-V ownership: PASS");
}

/* Completes actual ordinary creation framing and decodes one exact acknowledged native identity. */
static int
input_created(
	struct bcm2711_vulkan_session *session,
	struct vulkan_writer *writer,
	uint64_t identity)
{
	struct vulkan_reader reader;
	uint64_t returned;
	uint32_t status;
	int error;

	/* Native creation uses the same null allocator and typed output identity as the actual client helper. */
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, identity);
	error = execute(session, writer, &reader);
	assert(error == 0);
	status = vulkan_read_u32(&reader);
	assert(vulkan_read_u64(&reader) == 1);
	returned = vulkan_read_u64(&reader);
	if (status == VK_SUCCESS)
		assert(returned == identity);
	else
		assert(returned == 0);

	/* Succeeded: the caller sees an explicit Vulkan creation outcome after complete reply decoding. */
	return (int)status;
}

/* Exercises actual descriptor/pipeline layout encoders, canonical bindings and immutable ownership through parent identity retirement. */
static void
layout_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_object client_sampler;
	struct vulkan_object client_layout;
	struct bcm2711_vulkan_object *sampler;
	struct bcm2711_vulkan_object *layout;
	struct bcm2711_vulkan_object *pipeline;
	struct bcm2711_vulkan_set_layout *set;
	struct bcm2711_vulkan_pipeline_layout *interface;
	VkSamplerCreateInfo sampler_info;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo pipeline_info;
	VkPushConstantRange ranges[2];
	VkSampler immutable;
	VkDescriptorSetLayout selected;
	uint8_t wire[1024];
	unsigned baseline;
	int error;

	/* One actual sampler supplies the descriptor's independently retained immutable edge. */
	baseline = allocations;
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SAMPLER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSamplerCreateInfo(&writer, &sampler_info);
	error = input_created(session, &writer, 80);
	assert(error == VK_SUCCESS);
	sampler = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SAMPLER, 80);
	assert(sampler != NULL && sampler->references == 1);
	memset(&client_sampler, 0, sizeof(client_sampler));
	client_sampler.wire_id = 80;
	immutable = (VkSampler)(uintptr_t)&client_sampler;

	/* Input binding order is independent of native canonical order, and immutable sampler ownership survives source identity removal. */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 7;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[0].pImmutableSamplers = &immutable;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 2;
	set_info.pBindings = bindings;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&writer, &set_info);
	error = input_created(session, &writer, 81);
	assert(error == VK_SUCCESS && sampler->references == 2);
	layout = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, 81);
	assert(layout != NULL);
	set = layout->payload;
	assert(set->count == 2 && set->bindings[0].number == 1 && set->bindings[1].number == 7);
	assert(set->textures == 1 && set->uniforms == 1 && set->bindings[1].immutable == sampler);
	memset(&client_layout, 0, sizeof(client_layout));
	client_layout.wire_id = 81;
	selected = (VkDescriptorSetLayout)(uintptr_t)&client_layout;

	/* Exact per-stage push ranges and copied set interfaces are retained in the native pipeline layout. */
	memset(ranges, 0, sizeof(ranges));
	ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	ranges[0].size = 32;
	ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	ranges[1].offset = 32;
	ranges[1].size = 96;
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_info.setLayoutCount = 1;
	pipeline_info.pSetLayouts = &selected;
	pipeline_info.pushConstantRangeCount = 2;
	pipeline_info.pPushConstantRanges = ranges;
	fail_after = 2;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_PIPELINE_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkPipelineLayoutCreateInfo(&writer, &pipeline_info);
	error = input_created(session, &writer, 82);
	assert(error == (int)VK_ERROR_OUT_OF_HOST_MEMORY && layout->references == 1);
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_PIPELINE_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkPipelineLayoutCreateInfo(&writer, &pipeline_info);
	error = input_created(session, &writer, 82);
	assert(error == VK_SUCCESS && layout->references == 2);
	pipeline = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, 82);
	assert(pipeline != NULL);
	interface = pipeline->payload;
	assert(interface->count == 1 && interface->sets[0] == layout);
	assert(interface->push[0] == VK_SHADER_STAGE_VERTEX_BIT && interface->push[7] == VK_SHADER_STAGE_VERTEX_BIT);
	assert(interface->push[8] == VK_SHADER_STAGE_FRAGMENT_BIT && interface->push[31] == VK_SHADER_STAGE_FRAGMENT_BIT);

	/* A malformed repeated-stage range never allocates or modifies an existing pipeline interface. */
	ranges[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_PIPELINE_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkPipelineLayoutCreateInfo(&writer, &pipeline_info);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 83);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && interface->push[31] == VK_SHADER_STAGE_FRAGMENT_BIT);

	/* A prepared pipeline-layout owner keeps the descriptor layout and immutable sampler after every public identity retires. */
	error = bcm2711_vulkan_object_retain(pipeline);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_SAMPLER, 80);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT, 81);
	destroy(session, GPU_OP_DESTROY_PIPELINE_LAYOUT, 82);
	assert(sampler->references == 1 && layout->references == 1 && pipeline->references == 1);
	error = bcm2711_vulkan_object_release(pipeline);
	assert(error == 0 && allocations == baseline);
	puts("WS141 Vulkan actual canonical descriptor/pipeline layout/immutable dependencies/push permissions: PASS");
}

/* Exercises complete descriptor batches, exact capacity accounting and retained old set ownership across pool reset/destruction. */
static void
pool_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct bcm2711_vulkan_object *layout;
	struct bcm2711_vulkan_object *pool_object;
	struct bcm2711_vulkan_object *old;
	struct bcm2711_vulkan_object *remaining;
	struct bcm2711_vulkan_descriptor_pool *pool;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo layout_info;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	uint8_t wire[512];
	unsigned baseline;
	unsigned with_pool;
	int error;

	/* A standard single combined-image binding supplies the same per-set capacity used by Keiland. */
	baseline = allocations;
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&writer, &layout_info);
	error = input_created(session, &writer, 90);
	assert(error == VK_SUCCESS);
	layout = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, 90);
	assert(layout != NULL && layout->references == 1);

	/* The pool accepts Keiland's 512-set declaration while this bounded scenario uses only two texture slots. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 512;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorPoolCreateInfo(&writer, &pool_info);
	error = input_created(session, &writer, 92);
	assert(error == VK_SUCCESS);
	pool_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_POOL, 92);
	assert(pool_object != NULL);
	pool = pool_object->payload;
	assert(pool->maximum_sets == 512 && pool->maximum_textures == 2 && pool->sets == 0);
	with_pool = allocations;

	/* Failure at the second set's registry allocation withdraws the first identity and restores both charges and all parent edges. */
	fail_after = 4;
	error = sets_allocate(session, 92, 90, 93);
	assert(error == (int)VK_ERROR_OUT_OF_HOST_MEMORY && allocations == with_pool);
	old = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 93);
	remaining = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 94);
	assert(old == NULL && remaining == NULL && pool->sets == 0 && pool->textures == 0 && layout->references == 1);
	error = sets_allocate(session, 92, 90, 93);
	assert(error == VK_SUCCESS && pool->sets == 2 && pool->textures == 2 && layout->references == 3);
	old = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 93);
	assert(old != NULL);
	error = bcm2711_vulkan_object_retain(old);
	assert(error == 0);

	/* Free one exact selected set before reset withdraws the retained old set's remaining public identity. */
	begin(&writer, wire, sizeof(wire), GPU_OP_FREE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 92);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 94);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && pool->sets == 1);
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_DESCRIPTOR_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 92);
	vulkan_write_u32(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(old->references == 1 && !old->published && pool->sets == 1 && pool->textures == 1);
	error = sets_allocate(session, 92, 90, 95);
	assert(error == (int)VK_ERROR_OUT_OF_POOL_MEMORY && pool->sets == 1);
	error = bcm2711_vulkan_object_release(old);
	assert(error == 0 && pool->sets == 0 && pool->textures == 0 && allocations == with_pool);

	/* A fresh batch can use capacity only after old prepared owners retire, without reusing their typed object pointers. */
	error = sets_allocate(session, 92, 90, 95);
	assert(error == VK_SUCCESS);
	old = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 95);
	assert(old != NULL);
	error = bcm2711_vulkan_object_retain(old);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT, 90);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_POOL, 92);
	assert(old->references == 1 && pool_object->references == 1 && layout->references == 1);
	assert(pool->sets == 1 && pool->textures == 1);
	error = bcm2711_vulkan_object_release(old);
	assert(error == 0 && allocations == baseline);
	puts("WS141 Vulkan actual pool/set batch rollback/free/reset/retained capacity ownership: PASS");
}

/* Encodes the real allocation record and verifies both outputs of one complete two-set native batch. */
static int
sets_allocate(
	struct bcm2711_vulkan_session *session,
	uint64_t pool,
	uint64_t layout,
	uint64_t first)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_object client_pool;
	struct vulkan_object client_layout;
	VkDescriptorSetLayout layouts[2];
	VkDescriptorSetAllocateInfo info;
	uint8_t wire[512];
	uint32_t status;
	uint64_t count;
	int error;

	/* Actual local handle conversion supplies exact native pool/layout identities to the standard encoder. */
	memset(&client_pool, 0, sizeof(client_pool));
	memset(&client_layout, 0, sizeof(client_layout));
	client_pool.wire_id = pool;
	client_layout.wire_id = layout;
	layouts[0] = (VkDescriptorSetLayout)(uintptr_t)&client_layout;
	layouts[1] = layouts[0];
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = (VkDescriptorPool)(uintptr_t)&client_pool;
	info.descriptorSetCount = 2;
	info.pSetLayouts = layouts;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorSetAllocateInfo(&writer, &info);
	vulkan_write_u64(&writer, 2);
	vulkan_write_u64(&writer, first);
	vulkan_write_u64(&writer, first + 1U);
	error = execute(session, &writer, &reader);
	assert(error == 0);
	status = vulkan_read_u32(&reader);
	count = vulkan_read_u64(&reader);
	if (status == VK_SUCCESS) {
		assert(count == 2 && vulkan_read_u64(&reader) == first && vulkan_read_u64(&reader) == first + 1U);
	} else {
		assert(count == 0);
	}

	/* Succeeded: the exact complete batch outcome follows the actual client's allocation framing. */
	return (int)status;
}

/* Exercises complete ordered native updates, rollback and draw snapshots using actual client framing and handle codecs. */
static void
descriptor_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_object client_image;
	struct vulkan_object client_sampler;
	struct vulkan_object client_layouts[2];
	struct vulkan_object client_pool;
	struct vulkan_object client_sets[2];
	struct vulkan_object client_buffer;
	struct bcm2711_vulkan_object *view;
	struct bcm2711_vulkan_object *second_view;
	struct bcm2711_vulkan_object *sampler;
	struct bcm2711_vulkan_object *buffer;
	struct bcm2711_vulkan_object *set_object;
	struct bcm2711_vulkan_descriptor_set *immutable_set;
	struct bcm2711_vulkan_descriptor_set *mutable_set;
	struct bcm2711_vulkan_descriptor snapshot;
	struct bcm2711_vulkan_descriptor uniform_snapshot;
	struct bcm2711_vulkan_descriptor overflow_snapshot;
	struct bcm2711_buffer *blob;
	struct gpu_blob_create blob_request;
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkSamplerCreateInfo sampler_info;
	VkBufferCreateInfo buffer_info;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo layout_info;
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetLayout layout_handles[2];
	VkDescriptorSetAllocateInfo allocation;
	VkDescriptorBufferInfo uniform;
	VkCopyDescriptorSet copy;
	VkSampler immutable;
	uint8_t wire[2048];
	unsigned baseline;
	unsigned with_inputs;
	uint32_t references;
	uint32_t opcode;
	uint32_t index;
	int error;

	/* A single coherent allocation backs both a sampled image and an exact 65-byte uniform buffer. */
	baseline = allocations;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	image_info.extent.width = 16;
	image_info.extent.height = 8;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, &image_info);
	error = input_created(session, &writer, 100);
	assert(error == VK_SUCCESS);
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = 65;
	buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_BUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferCreateInfo(&writer, &buffer_info);
	error = input_created(session, &writer, 101);
	assert(error == VK_SUCCESS);
	error = memory_allocate(session, 102, 8192, 0, 0);
	assert(error == VK_SUCCESS);
	memset(&blob_request, 0, sizeof(blob_request));
	blob_request.blob_id = 102;
	blob_request.bytes = 8192;
	blob_request.flags = GPU_BLOB_MAPPABLE;
	error = bcm2711_vulkan_memory_blob(session, &blob_request, NULL, &blob);
	assert(error == 0);
	for (index = 0; index < 2; index++) {
		opcode = GPU_OP_BIND_IMAGE_MEMORY;
		if (index != 0)
			opcode = GPU_OP_BIND_BUFFER_MEMORY;
		begin(&writer, wire, sizeof(wire), opcode, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 100 + index);
		vulkan_write_u64(&writer, 102);
		vulkan_write_u64(&writer, index * 512U);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	}

	/* Two actual view identities share the image, so snapshot tests can distinguish old and new immutable inputs. */
	memset(&client_image, 0, sizeof(client_image));
	client_image.wire_id = 100;
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = (VkImage)(uintptr_t)&client_image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = image_info.format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE_VIEW, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkImageViewCreateInfo(&writer, &view_info);
		error = input_created(session, &writer, 103 + index);
		assert(error == VK_SUCCESS);
	}

	/* Exercise compatible clear/load targets while the exact bound image/view identities are still available. */
	target_test(session);

	/* Distinct sampler identities let copies demonstrate that destination immutable semantics override source mutable state. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SAMPLER, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkSamplerCreateInfo(&writer, &sampler_info);
		error = input_created(session, &writer, 105 + index);
		assert(error == VK_SUCCESS);
	}

	/* Both layouts have canonical image/uniform slots, with only the first layout retaining its immutable sampler. */
	memset(&client_sampler, 0, sizeof(client_sampler));
	client_sampler.wire_id = 105;
	immutable = (VkSampler)(uintptr_t)&client_sampler;
	memset(bindings, 0, sizeof(bindings));
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[0].pImmutableSamplers = &immutable;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 2;
	layout_info.pBindings = bindings;
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkDescriptorSetLayoutCreateInfo(&writer, &layout_info);
		error = input_created(session, &writer, 110 + index);
		assert(error == VK_SUCCESS);
		bindings[0].pImmutableSamplers = NULL;
	}

	/* One pool allocates the two distinct interfaces through the real client record encoder. */
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = 2;
	sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[1].descriptorCount = 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 2;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = sizes;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorPoolCreateInfo(&writer, &pool_info);
	error = input_created(session, &writer, 112);
	assert(error == VK_SUCCESS);
	memset(client_layouts, 0, sizeof(client_layouts));
	memset(&client_pool, 0, sizeof(client_pool));
	client_pool.wire_id = 112;
	for (index = 0; index < 2; index++) {
		client_layouts[index].wire_id = 110 + index;
		layout_handles[index] = (VkDescriptorSetLayout)(uintptr_t)&client_layouts[index];
	}

	/* Actual set allocation creates one immutable and one mutable sampled-image interface. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocation.descriptorPool = (VkDescriptorPool)(uintptr_t)&client_pool;
	allocation.descriptorSetCount = 2;
	allocation.pSetLayouts = layout_handles;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorSetAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 2);
	vulkan_write_u64(&writer, 113);
	vulkan_write_u64(&writer, 114);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	set_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 113);
	assert(set_object != NULL);
	immutable_set = set_object->payload;
	set_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 114);
	assert(set_object != NULL);
	mutable_set = set_object->payload;
	view = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, 103);
	second_view = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, 104);
	sampler = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SAMPLER, 105);
	buffer = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, 101);
	with_inputs = allocations;

	/* A write followed by a copy in one command sees the staged view and keeps the destination's immutable sampler. */
	memset(client_sets, 0, sizeof(client_sets));
	client_sets[0].wire_id = 113;
	client_sets[1].wire_id = 114;
	memset(&copy, 0, sizeof(copy));
	copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
	copy.srcSet = (VkDescriptorSet)(uintptr_t)&client_sets[1];
	copy.dstSet = (VkDescriptorSet)(uintptr_t)&client_sets[0];
	copy.descriptorCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 114, 106, 103);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCopyDescriptorSet(&writer, &copy);
	error = execute(session, &writer, &reader);
	assert(error == 0 && allocations == with_inputs);
	assert(mutable_set->bindings[0].view == view && mutable_set->bindings[0].sampler != sampler);
	assert(immutable_set->bindings[0].view == view && immutable_set->bindings[0].sampler == sampler);
	memset(&snapshot, 0, sizeof(snapshot));
	error = bcm2711_vulkan_descriptor_clone(&immutable_set->bindings[0], &snapshot);
	assert(error == 0);

	/* Overflow during later sampler acquisition must return the earlier view edge without publishing a partial snapshot. */
	memset(&overflow_snapshot, 0, sizeof(overflow_snapshot));
	references = sampler->references;
	sampler->references = UINT32_MAX;
	error = bcm2711_vulkan_descriptor_clone(&immutable_set->bindings[0], &overflow_snapshot);
	sampler->references = references;
	assert(error == EOVERFLOW && overflow_snapshot.view == NULL && view->references == 4);

	/* Failure in a later copy rolls back an earlier valid image replacement without changing either live set. */
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 114, 106, 104);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	copy.dstBinding = 7;
	vulkan_encode_VkCopyDescriptorSet(&writer, &copy);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && allocations == with_inputs);
	assert(mutable_set->bindings[0].view == view && immutable_set->bindings[0].view == view);

	/* Transaction allocation failure similarly leaves exact view/sampler owners unchanged. */
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 113, 0, 104);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	fail_after = 1;
	error = execute(session, &writer, &reader);
	assert(error == ENOMEM && allocations == with_inputs && immutable_set->bindings[0].view == view);

	/* Actual buffer-info encoding resolves VK_WHOLE_SIZE against logical bytes, excluding padded allocation storage. */
	memset(&client_buffer, 0, sizeof(client_buffer));
	client_buffer.wire_id = 101;
	memset(&uniform, 0, sizeof(uniform));
	uniform.buffer = (VkBuffer)(uintptr_t)&client_buffer;
	uniform.offset = 4;
	uniform.range = VK_WHOLE_SIZE;
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 2);
	vulkan_write_u64(&writer, 2);
	encode_image_write(&writer, 113, 0, 104);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 113);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u32(&writer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorBufferInfo(&writer, &uniform);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && allocations == with_inputs);
	assert(immutable_set->bindings[0].view == second_view && snapshot.view == view);
	assert(immutable_set->bindings[1].buffer == buffer && immutable_set->bindings[1].offset == 4 && immutable_set->bindings[1].bytes == 61);
	memset(&uniform_snapshot, 0, sizeof(uniform_snapshot));
	error = bcm2711_vulkan_descriptor_clone(&immutable_set->bindings[1], &uniform_snapshot);
	assert(error == 0);

	/* Reset and public resource destruction retire mutable sets but preserve both exact prepared snapshots and their backing memory. */
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_DESCRIPTOR_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 112);
	vulkan_write_u32(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_POOL, 112);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT, 110);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT, 111);
	destroy(session, GPU_OP_DESTROY_IMAGE_VIEW, 103);
	destroy(session, GPU_OP_DESTROY_IMAGE_VIEW, 104);
	destroy(session, GPU_OP_DESTROY_SAMPLER, 105);
	destroy(session, GPU_OP_DESTROY_SAMPLER, 106);
	destroy(session, GPU_OP_DESTROY_IMAGE, 100);
	destroy(session, GPU_OP_DESTROY_BUFFER, 101);
	memory_free(session, 102);
	bcm2711_buffer_release(blob);
	assert(snapshot.view == view && snapshot.sampler == sampler && view->references == 1 && sampler->references == 1);
	assert(uniform_snapshot.buffer == buffer && buffer->references == 1 && session->render->device->vulkan_memory_bytes == 8192);
	error = bcm2711_vulkan_descriptor_release(&snapshot);
	assert(error == 0 && session->render->device->vulkan_memory_bytes == 8192);
	error = bcm2711_vulkan_descriptor_release(&uniform_snapshot);
	assert(error == 0 && allocations == baseline && session->render->device->vulkan_memory_bytes == 0);
	puts("WS141 Vulkan ordered writes/copies/immutable override/rollback/exact uniform range/independent draw snapshot: PASS");
}

/* Follows the actual client descriptor_write selected-field framing for one combined-image binding. */
static void
encode_image_write(
	struct vulkan_writer *writer,
	uint64_t set,
	uint64_t sampler,
	uint64_t view)
{
	/* The native wire selects a single image payload and explicitly absent buffer/texel arrays. */
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, set);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 1);
	vulkan_write_u32(writer, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, sampler);
	vulkan_write_u64(writer, view);
	vulkan_write_u32(writer, VK_IMAGE_LAYOUT_GENERAL);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 0);

	/* Succeeded: one complete descriptor write is encoded without an application pointer. */
	return;
}

/* Exercises actual Keiland clear/load/backdrop pass records, bound framebuffer validation and independent target ownership. */
static void
target_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_object client_pass;
	struct vulkan_object client_view;
	struct bcm2711_vulkan_object *clear_object;
	struct bcm2711_vulkan_object *load_object;
	struct bcm2711_vulkan_object *framebuffer_object;
	struct bcm2711_vulkan_object *view_object;
	struct bcm2711_vulkan_pass *clear;
	struct bcm2711_vulkan_pass *load;
	struct bcm2711_vulkan_framebuffer *target;
	VkAttachmentDescription attachment;
	VkAttachmentReference colour;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo pass_info;
	VkFramebufferCreateInfo framebuffer_info;
	VkImageView attachment_handle;
	uint8_t wire[1024];
	unsigned baseline;
	unsigned with_passes;
	uint32_t references;
	int error;

	/* The actual client encoder maps Keiland's present layout to GENERAL while preserving its clear/store semantics. */
	baseline = allocations;
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	memset(&colour, 0, sizeof(colour));
	colour.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colour;
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass_info.attachmentCount = 1;
	pass_info.pAttachments = &attachment;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	pass_info.dependencyCount = 1;
	pass_info.pDependencies = dependencies;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_RENDER_PASS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkRenderPassCreateInfo(&writer, &pass_info);
	error = input_created(session, &writer, 120);
	assert(error == VK_SUCCESS);
	clear_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, 120);
	assert(clear_object != NULL);
	clear = clear_object->payload;
	assert(clear->colour.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && clear->colour.finalLayout == VK_IMAGE_LAYOUT_GENERAL);

	/* A compatible loading pass retains a different lifecycle without changing colour-subpass compatibility. */
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_RENDER_PASS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkRenderPassCreateInfo(&writer, &pass_info);
	error = input_created(session, &writer, 121);
	assert(error == VK_SUCCESS);
	load_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, 121);
	assert(load_object != NULL);
	load = load_object->payload;
	assert(load->colour.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && load->colour.initialLayout == VK_IMAGE_LAYOUT_GENERAL);
	error = bcm2711_vulkan_pass_compatible(clear, load);
	assert(error == 0);

	/* The actual backdrop's two external dependencies and sampled final layout remain independently copied native fields. */
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	pass_info.dependencyCount = 2;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_RENDER_PASS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkRenderPassCreateInfo(&writer, &pass_info);
	error = input_created(session, &writer, 123);
	assert(error == VK_SUCCESS);
	load_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, 123);
	assert(load_object != NULL);
	load = load_object->payload;
	assert(load->count == 2 && load->dependencies[1].dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
	assert(load->colour.finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	destroy(session, GPU_OP_DESTROY_RENDER_PASS, 123);

	/* Compile actual Keiland programs against this same-device clear pass before its public identity retires. */
	pipeline_test(session);

	/* One-pixel API granularity allows a caller to choose damage rectangles independently of native tile size. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_RENDER_AREA_GRANULARITY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 120);
	vulkan_write_u64(&writer, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u64(&reader) == 1 && vulkan_read_u32(&reader) == 1 && vulkan_read_u32(&reader) == 1);
	with_passes = allocations;

	/* Actual handle conversion supplies the same-device pass and full-colour view used by Keiland framebuffer creation. */
	memset(&client_pass, 0, sizeof(client_pass));
	memset(&client_view, 0, sizeof(client_view));
	client_pass.wire_id = 120;
	client_view.wire_id = 103;
	attachment_handle = (VkImageView)(uintptr_t)&client_view;
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = (VkRenderPass)(uintptr_t)&client_pass;
	framebuffer_info.attachmentCount = 1;
	framebuffer_info.pAttachments = &attachment_handle;
	framebuffer_info.width = 16;
	framebuffer_info.height = 8;
	framebuffer_info.layers = 1;
	view_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, 103);
	assert(view_object != NULL);
	references = view_object->references;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_FRAMEBUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkFramebufferCreateInfo(&writer, &framebuffer_info);
	fail_after = 2;
	error = input_created(session, &writer, 122);
	assert(error == (int)VK_ERROR_OUT_OF_HOST_MEMORY && allocations == with_passes);
	assert(view_object->references == references && clear_object->references == 1);

	/* Extent mismatch refuses creation without retaining target dependencies or acknowledging a framebuffer identity. */
	framebuffer_info.width = 17;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_FRAMEBUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkFramebufferCreateInfo(&writer, &framebuffer_info);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 122);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && allocations == with_passes && view_object->references == references);
	framebuffer_info.width = 16;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_FRAMEBUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkFramebufferCreateInfo(&writer, &framebuffer_info);
	error = input_created(session, &writer, 122);
	assert(error == VK_SUCCESS);
	framebuffer_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FRAMEBUFFER, 122);
	assert(framebuffer_object != NULL);
	target = framebuffer_object->payload;
	assert(target->owner.parent == clear_object && target->view == view_object && target->width == 16 && target->height == 8);
	error = bcm2711_vulkan_object_retain(framebuffer_object);
	assert(error == 0);

	/* Prepared framebuffer ownership survives both public framebuffer and pass destruction with the exact view and attachment semantics. */
	destroy(session, GPU_OP_DESTROY_FRAMEBUFFER, 122);
	destroy(session, GPU_OP_DESTROY_RENDER_PASS, 120);
	destroy(session, GPU_OP_DESTROY_RENDER_PASS, 121);
	assert(framebuffer_object->references == 1 && clear_object->references == 1 && view_object->references == references + 1);
	assert(target->owner.parent == clear_object && clear->colour.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	error = bcm2711_vulkan_object_release(framebuffer_object);
	assert(error == 0 && allocations == baseline && view_object->references == references);
	puts("WS141 Vulkan actual clear/load/backdrop passes/target compatibility/framebuffer OOM/retained native target: PASS");
}

/* Exercises actual module bytes and native compiler/interface ownership before the wire batch router publishes pipelines. */
static void
pipeline_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *layout;
	struct bcm2711_vulkan_pipeline *pipeline;
	struct vulkan_object client_set;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_info;
	VkDescriptorSetLayout set_handle;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout_info;
	VkShaderModuleCreateInfo module;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription vertex_binding;
	VkVertexInputAttributeDescription attribute;
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo samples;
	VkPipelineColorBlendAttachmentState attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamic_commands[2];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo info;
	uint8_t wire[8192];
	unsigned baseline;
	unsigned with_inputs;
	uint32_t index;
	int error;

	/* Actual client layout records supply the quad's fragment sampler and exact 32-byte vertex push range. */
	baseline = allocations;
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 1;
	set_info.pBindings = &binding;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&writer, &set_info);
	error = input_created(session, &writer, 130);
	assert(error == VK_SUCCESS);
	memset(&client_set, 0, sizeof(client_set));
	client_set.wire_id = 130;
	set_handle = (VkDescriptorSetLayout)(uintptr_t)&client_set;
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.size = 32;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_handle;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_PIPELINE_LAYOUT, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkPipelineLayoutCreateInfo(&writer, &layout_info);
		error = input_created(session, &writer, 131 + index);
		assert(error == VK_SUCCESS);
		push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	/* Ordinary native module creation owns exact immutable Keiland source arrays independently of the stream arena. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_quad_vert);
	module.pCode = kwl_quad_vert;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SHADER_MODULE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkShaderModuleCreateInfo(&writer, &module);
	error = input_created(session, &writer, 133);
	assert(error == VK_SUCCESS);
	module.codeSize = sizeof(kwl_quad_frag);
	module.pCode = kwl_quad_frag;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SHADER_MODULE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkShaderModuleCreateInfo(&writer, &module);
	error = input_created(session, &writer, 134);
	assert(error == VK_SUCCESS);

	/* These fields are decoded native IDs, not client pointers; the backend accepts exactly the records the future wire decoder produces. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = (VkShaderModule)(uintptr_t)133;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = (VkShaderModule)(uintptr_t)134;
	stages[1].pName = "main";
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.stride = 8;
	memset(&attribute, 0, sizeof(attribute));
	attribute.format = VK_FORMAT_R32G32_SFLOAT;
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1;
	input.pVertexBindingDescriptions = &vertex_binding;
	input.vertexAttributeDescriptionCount = 1;
	input.pVertexAttributeDescriptions = &attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.lineWidth = 1.0f;
	memset(&samples, 0, sizeof(samples));
	samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&attachment, 0, sizeof(attachment));
	attachment.blendEnable = VK_TRUE;
	attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;
	dynamic_commands[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamic_commands[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_commands;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &samples;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = (VkPipelineLayout)(uintptr_t)131;
	info.renderPass = (VkRenderPass)(uintptr_t)120;
	info.basePipelineIndex = -1;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, 30);
	layout = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, 131);
	assert(device != NULL && layout != NULL);
	with_inputs = allocations;

	/* A declared push range invisible to the vertex stage is refused after partial compilation without leaking any parent or program. */
	info.layout = (VkPipelineLayout)(uintptr_t)132;
	error = bcm2711_vulkan_pipeline_build(session, device, &info, &pipeline);
	assert(error == EINVAL && pipeline == NULL && allocations == with_inputs && layout->references == 1);
	info.layout = (VkPipelineLayout)(uintptr_t)131;
	fail_after = 2;
	error = bcm2711_vulkan_pipeline_build(session, device, &info, &pipeline);
	assert(error == ENOMEM && pipeline == NULL && allocations == with_inputs && layout->references == 1);

	/* A scalar-only vertex attribute cannot supply the actual quad shader's second input component. */
	attribute.format = VK_FORMAT_R32_SFLOAT;
	error = bcm2711_vulkan_pipeline_build(session, device, &info, &pipeline);
	assert(error == ENOTSUP && pipeline == NULL && allocations == with_inputs && layout->references == 1);
	attribute.format = VK_FORMAT_R32G32_SFLOAT;
	error = bcm2711_vulkan_pipeline_build(session, device, &info, &pipeline);
	assert(error == 0 && pipeline != NULL && layout->references == 2);
	assert(pipeline->blend && pipeline->binding_count == 1 && pipeline->attributes[0].format == VK_FORMAT_R32G32_SFLOAT);
	assert(pipeline->programs[0]->vpm_output_words == 6 && pipeline->programs[1]->input_count == 2 && pipeline->programs[2]->varying_count == 2);
	assert(pipeline->programs[2]->code_count != 0 && pipeline->programs[2]->uniform_count != 0);

	/* Source modules and public layout identities may retire without invalidating independently owned native code/interface metadata. */
	destroy(session, GPU_OP_DESTROY_SHADER_MODULE, 133);
	destroy(session, GPU_OP_DESTROY_SHADER_MODULE, 134);
	destroy(session, GPU_OP_DESTROY_PIPELINE_LAYOUT, 131);
	destroy(session, GPU_OP_DESTROY_PIPELINE_LAYOUT, 132);
	destroy(session, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT, 130);
	assert(layout->references == 1 && !layout->published && pipeline->programs[2]->code_count != 0);
	error = bcm2711_vulkan_pipeline_release(session, pipeline);
	assert(error == 0 && allocations == baseline);
	puts("WS141 Vulkan actual quad compiler/push visibility/attribute interface/OOM/compiled pipeline ownership: PASS");
}
