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
	if (session != reply_resource.owner || identity != 1)
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
	assert(buffer == &reply_buffer && buffer->references != 0);
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
	assert(buffer == &reply_buffer && buffer->references > 1);
	buffer->references--;
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
	/* Fixed fixture storage uses actual primitives without invoking an unrelated local Vulkan object allocator. */
	memset(writer, 0, sizeof(*writer));
	writer->data = storage;
	writer->capacity = capacity;
	vulkan_write_u32(writer, GPU_OP_SET_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, sizeof(reply_storage));
	vulkan_write_u32(writer, opcode);
	vulkan_write_u32(writer, requested);
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

/* Destroys an actual typed native root through its no-reply Vulkan command. */
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
	begin(&writer, wire, sizeof(wire), opcode, 0);
	vulkan_write_u64(&writer, identity);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0);
}
