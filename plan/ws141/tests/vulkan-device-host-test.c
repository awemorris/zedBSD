/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Real client record encoding/decoding crosses the native transport and typed root/query implementation. */
#include <assert.h>
#include <kern/dcache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include "userland/desktop/libvulkan/internal.h"
#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-device.h"
#include "drivers/gpu/bcm2711/vulkan-sync.h"
#include "drivers/gpu/bcm2711/vulkan-queue.h"
#include "drivers/gpu/bcm2711/vulkan-memory.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"
#include "drivers/gpu/bcm2711/vulkan-input.h"
#include "drivers/gpu/bcm2711/vulkan-layout.h"
#include "drivers/gpu/bcm2711/vulkan-descriptor.h"
#include "drivers/gpu/bcm2711/vulkan-target.h"
#include "drivers/gpu/bcm2711/vulkan-pipeline.h"
#include "drivers/gpu/bcm2711/vulkan-command.h"
#include "drivers/gpu/bcm2711/vulkan-record.h"
#include "drivers/gpu/bcm2711/vulkan-draw.h"
#include "drivers/gpu/bcm2711/vulkan-prepared.h"
#include "drivers/gpu/bcm2711/vulkan-uniform.h"
#include "drivers/gpu/bcm2711/vulkan-native-draw.h"
#include "drivers/gpu/bcm2711/vulkan-native-pass.h"
#include "drivers/gpu/bcm2711/vulkan-native-job.h"
#include "drivers/gpu/bcm2711/vulkan-barrier.h"
#include "drivers/gpu/bcm2711/vulkan-native-image.h"
#include "drivers/gpu/bcm2711/vulkan-buffer-copy.h"
#include "userland/desktop/wayland/shaders.h"

/* Synthetic UBO metadata borrows actual retained buffer backing; it is never published as a client pipeline or DMA job. */
struct uniform_fixture {
	struct bcm2711_vulkan_prepared_event event;
	struct bcm2711_vulkan_pipeline pipeline;
	struct bcm2711_vulkan_pipeline_layout layout;
	struct bcm2711_vulkan_set_layout set;
	struct bcm2711_vulkan_object layout_object;
	struct bcm2711_vulkan_object set_object;
	struct bcm2711_shader_binary program;
	struct bcm2711_shader_uniform uniforms[3];
};

/* Synthetic clear/area events borrow actual pending primary ownership without publishing replacement client records. */
struct execute_fixture {
	struct bcm2711_vulkan_record record;
	struct bcm2711_vulkan_prepared_event events[3];
};

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

/* Unique numerical physical pages model independent cached upload allocation placement, without physical cache or GPU execution. */
static uint64_t next_native_physical = 0x200000U;

/* Complete cache-clean observations distinguish device-visible upload preparation from a GPU launch. */
static unsigned native_cleans;

/* The explicit native-runner fixture reports selected completion outcomes; it never executes physical DMA or QPU work. */
static int native_execute_error;

/* Selected synthetic retirement observations test whole-owner preservation, without claiming a checked physical reset. */
static bool native_execute_retired;

/* Only expected executor handoffs increment this fixture counter; refused replay or preflight must leave it unchanged. */
static unsigned native_execute_calls;

/* Device publication barriers are observed independently of the mocked native runner in this single-thread host fixture. */
static unsigned native_write_barriers;

/* Output-visibility barriers are observed only after successful retired native-runner handoff. */
static unsigned native_read_barriers;

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

void ws141_client_encode_recording(struct vulkan_writer *writer, uint64_t command_id);
void ws141_client_encode_transfer(struct vulkan_writer *writer, uint64_t command_id, uint64_t source_id, uint64_t destination_id, VkImageLayout source_layout, VkImageLayout destination_layout, const VkImageCopy *copies, const VkImageBlit *blits, uint32_t count, VkFilter filter);
void ws141_client_encode_clear(struct vulkan_writer *writer, uint64_t command_id, uint64_t image_id, VkImageLayout layout, const VkImageSubresourceRange *ranges, uint32_t count);
void ws141_client_encode_barrier(struct vulkan_writer *writer, uint64_t command_id, uint64_t buffer_id, const uint64_t *image_ids, const VkImageLayout *layouts);

void ws141_client_encode_graphics(struct vulkan_writer *writer, const VkGraphicsPipelineCreateInfo *source);

static void native_job_test(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *command);
static void native_execute_test(struct bcm2711_vulkan_session *session, const struct bcm2711_vulkan_prepared_event *begin);
static void native_pass_test(struct bcm2711_vulkan_session *session, const struct bcm2711_vulkan_prepared_event *begin);
static void native_draw_test(struct bcm2711_vulkan_session *session, const struct bcm2711_vulkan_prepared_event *event);
static void uniform_test(struct bcm2711_vulkan_session *session, const struct bcm2711_vulkan_prepared_event *event);
static void prepared_test(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *command);
static void draw_test(struct bcm2711_vulkan_command_buffer *command);
static int draw_observe(void *payload, const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);
static void record_test(struct bcm2711_vulkan_session *session);
void ws141_client_encode_buffer_copy(struct vulkan_writer *writer, uint64_t command_id, uint64_t source_id, uint64_t destination_id, const VkBufferCopy *regions, uint32_t count);
void ws141_client_encode_raster_copy(struct vulkan_writer *writer, uint64_t command_id, uint64_t buffer_id, uint64_t image_id, VkImageLayout layout, const VkBufferImageCopy *regions, uint32_t count, VkBool32 upload);
extern void ws141_client_encode_submit(struct vulkan_writer *writer, uint64_t command_id, uint64_t semaphore_id, uint64_t fence_id, uint32_t mode);
static void queue_test(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *command);
static void raster_copy_test(struct bcm2711_vulkan_session *session);
static void buffer_copy_test(struct bcm2711_vulkan_session *session);
static void transfer_test(struct bcm2711_vulkan_session *session);
static void clear_test(struct bcm2711_vulkan_session *session);
static void barrier_test(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *primary);
static void command_test(struct bcm2711_vulkan_session *session);
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
static void sync_test(struct bcm2711_vulkan_session *session);
static void memory_test(struct bcm2711_vulkan_session *session, struct bcm2711_render_session *render);
static void external_barrier_test(struct bcm2711_vulkan_object *memory_object);
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
	if (buffer->references == 0) {
		/* Independently allocated cached fixture runs retire their CPU storage after their last actual mapping owner. */
		if (!buffer->uncached)
			kern_free(buffer->address);
		kern_free(buffer);
	}
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
 * Supplies independently owned cached upload RAM to actual native mapping and draw preparation source.
 */
int
bcm2711_buffer_create(
	uint64_t bytes,
	uint64_t limit,
	size_t alignment,
	struct bcm2711_buffer **result)
{
	struct bcm2711_buffer *buffer;

	/* Actual host CPU intervals are independent; numerical PA reachability is not physical device validation. */
	*result = NULL;
	assert(limit == 0xffffffffU && alignment == 4096 && bytes <= 1024U * 1024U);
	buffer = kern_calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;
	buffer->bytes = bytes;
	buffer->memory.size = (bytes + 4095U) & ~4095ULL;
	buffer->address = kern_calloc(1, (size_t)buffer->memory.size);
	if (buffer->address == NULL) {
		kern_free(buffer);
		return ENOMEM;
	}

	/* Distinct mapped PTEs cannot alias another fixture upload's independent source storage. */
	buffer->memory.paddr = next_native_physical;
	next_native_physical += buffer->memory.size;
	buffer->references = 1;
	*result = buffer;

	/* Succeeded: actual mapping source acquires its own reference to this complete padded cached fixture run. */
	return 0;
}

/*
 * Observes complete padded cache-clean requests without claiming real host or device cache maintenance.
 */
void
kern_dcache_clean_range(
	const void *address,
	size_t bytes)
{
	/* No coherent fixture alias is treated as cached upload storage. */
	assert(address != NULL && address != backing_storage && bytes != 0 && (bytes & 4095U) == 0);
	native_cleans++;

	/* Succeeded: the complete cache-maintenance request has been observed. */
	return;
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
 * Supplies an explicit synthetic completion to the real private native-pass executor.
 *
 * This fixture validates the actual handoff and ownership result only; native
 * job IRQ/cache/MMIO behavior is covered by its separate hardware fixture.
 */
int
bcm2711_v3d_job_run(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_job *job,
	struct bcm2711_v3d_job_result *result)
{
	/* Only a real independently prepared native CL job may reach this explicit host completion source. */
	assert(engine != NULL && engine->hardware.ready && job->kind == BCM2711_V3D_JOB_CL);
	assert(job->command.cl.bin_end > job->command.cl.bin_start && job->command.cl.render_end > job->command.cl.render_start);
	assert(job->command.cl.overflow_count == 4 && job->command.cl.clean_output);
	native_execute_calls++;
	memset(result, 0, sizeof(*result));
	result->retired = native_execute_retired;

	/* Succeeded: the fixture reports its selected protocol outcome, never a physical retirement proof. */
	return native_execute_error;
}

/*
 * Observes the real executor's publication barrier without performing device I/O on the host.
 */
void
kern_io_write_barrier(
	void)
{
	/* The single-thread fixture counts the barrier independently of native runner calls. */
	native_write_barriers++;

	/* Succeeded: the fixture observed publication order. */
	return;
}

/*
 * Observes the real executor's output-read barrier without performing physical cache maintenance.
 */
void
kern_io_read_barrier(
	void)
{
	/* Only confirmed successful retirement should expose a subsequent FIFO CPU read. */
	native_read_barriers++;

	/* Succeeded: the fixture observed the output visibility boundary. */
	return;
}

/*
 * Observes the actual worker fault publication's wake without inventing a host scheduler or common completion callback.
 */
void
waitq_wake_one(
	struct wait_queue *queue)
{
	/* No host thread sleeps on this explicit single-thread queue fixture. */
	(void)queue;

	/* Succeeded: actual worker fault publication may proceed without physical scheduling. */
	return;
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
	engine.hardware.core_ident[1] = 2U << 28;
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
	sync_test(session);
	memory_test(session, &render);
	resource_test(session);
	input_test(session);
	layout_test(session);
	pool_test(session);
	descriptor_test(session);
	command_test(session);
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

/* Exercises actual primary lifecycle codecs and independent owners without pretending to execute native work. */
static void
command_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_object local;
	struct bcm2711_vulkan_object *pool_object;
	struct bcm2711_vulkan_object *first;
	struct bcm2711_vulkan_object *second;
	struct bcm2711_vulkan_command_pool *pool;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_command_buffer *other;
	VkCommandPoolCreateInfo info;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	uint8_t wire[4096];
	unsigned baseline;
	unsigned index;
	int error;

	/* Individual reset permission is separate from the always available whole-pool reset. */
	baseline = allocations;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandPoolCreateInfo(&writer, &info);
	error = input_created(session, &writer, 150);
	assert(error == 0);
	pool_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_POOL, 150);
	assert(pool_object != NULL);
	pool = pool_object->payload;

	/* Real client handle conversion and record widths select one ordinary primary pool. */
	memset(&local, 0, sizeof(local));
	local.kind = VULKAN_OBJECT_COMMAND_POOL;
	local.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&local;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 2;

	/* Second registry OOM must unwind the complete first output and both acquired parent graphs before retry. */
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
		vulkan_write_u64(&writer, 2);
		vulkan_write_u64(&writer, 151);
		vulkan_write_u64(&writer, 152);
		if (index == 0)
			fail_after = 4;
		error = execute(session, &writer, &reader);
		assert(error == 0);
		if (index == 0) {
			assert(vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY && vulkan_read_u64(&reader) == 0);
			first = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 151);
			second = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 152);
			assert(first == NULL && second == NULL && pool->children == NULL && pool_object->references == 1);
		} else {
			assert(vulkan_read_u32(&reader) == VK_SUCCESS && vulkan_read_u64(&reader) == 2);
			assert(vulkan_read_u64(&reader) == 151 && vulkan_read_u64(&reader) == 152);
		}
	}

	/* A complete actual Begin/End pair creates a legal empty executable command buffer. */
	first = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 151);
	second = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 152);
	assert(first != NULL && second != NULL && pool_object->references == 3);
	command = first->payload;
	other = second->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	recording.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 151);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && command->state == BCM2711_VULKAN_COMMAND_RECORDING);
	begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 151);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
	begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 151);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 151);
	vulkan_write_u32(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);

	/* Explicit pending fixture ownership must block whole-pool mutation and all-or-nothing free before any prefix changes. */
	error = bcm2711_vulkan_object_retain(second);
	assert(error == 0);
	other->pending = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u32(&writer, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
	begin(&writer, wire, sizeof(wire), GPU_OP_FREE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u32(&writer, 2);
	vulkan_write_u64(&writer, 2);
	vulkan_write_u64(&writer, 151);
	vulkan_write_u64(&writer, 152);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY && first->published && second->published);
	begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY && first->published && second->published && pool_object->published);
	other->pending = 0;
	error = bcm2711_vulkan_object_release(second);
	assert(error == 0);

	/* Pool reset returns children to initial state while retaining their original public IDs. */
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u32(&writer, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(first->published && second->published && command->state == BCM2711_VULKAN_COMMAND_INITIAL && other->state == BCM2711_VULKAN_COMMAND_INITIAL);

	/* Public destruction leaves the original pool alive behind an independently retained withdrawn child. */
	error = bcm2711_vulkan_object_retain(first);
	assert(error == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && !first->published && !pool_object->published && pool->children == command && pool_object->references == 1);
	error = bcm2711_vulkan_object_release(first);
	assert(error == 0 && allocations == baseline);
	puts("WS141 Vulkan actual client primary pool/buffer lifecycle, batch rollback and pending ownership: PASS");

	/* Succeeded: software lifecycle evidence preserves the separate native execution boundary. */
	return;
}

/* Checks complete real pipeline upload preparation, owned fetch/TMU snapshots, budget refusal and uncertainty retention without GPU execution. */
static void
native_draw_test(
	struct bcm2711_vulkan_session *session,
	const struct bcm2711_vulkan_prepared_event *event)
{
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_vulkan_prepared_event *changed;
	struct bcm2711_vulkan_pipeline *pipeline;
	struct bcm2711_shader_binary *programs;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_native_storage *storage;
	struct bcm2711_v3d_view *view;
	struct bcm2711_v3d_space *space;
	void *cpu;
	uint8_t *source;
	uint8_t *record;
	uint32_t address;
	uint32_t index;
	uint32_t byte;
	unsigned baseline;
	unsigned cleans;
	unsigned used;
	uint64_t available;
	uint64_t owned;
	int error;

	/* Known six-vertex vec2 data and a distinct sampled raster use actual coherent resource interval resolution. */
	space = &session->render->device->space;
	baseline = allocations;
	error = bcm2711_vulkan_resource_backing(event->vertices[0], 0, 48, &view, &address, &cpu);
	assert(error == 0);
	source = cpu;
	for (index = 0; index < 48; index++)
		source[index] = (uint8_t)(index + 1);
	image_view = event->descriptors[0][0].view->payload;
	image = image_view->owner.parent->payload;
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	assert(error == 0 && image->bytes == 512);
	memset(cpu, 0x75, 512);
	cleans = native_cleans;
	available = 1024U * 1024U;
	error = bcm2711_vulkan_native_draw_create(space, event, &available, &draw);
	assert(error == 0 && draw != NULL && draw->vertices == 6 && draw->attributes == 1 && draw->count == 11);
	owned = draw->bytes;
	assert(owned == 11U * 4096U && available == 1024U * 1024U - owned && native_cleans == cleans + 11);
	assert(draw->bindings[0][0].texture == draw->storage[1]->view->address);
	assert(draw->bindings[0][0].sampler == draw->storage[1]->view->address + 32);
	assert(draw->bin_bytes == BCM2711_NATIVE_BIN_BYTES && draw->bin[20] == 16 && draw->bin[22] == 8);
	address = draw->shader | draw->attributes;
	for (byte = 0; byte < 4; byte++)
		assert(draw->bin[102U + byte] == (uint8_t)(address >> (byte * 8U)));

	/* Every shader code upload contains the actual independently compiled instruction bytes. */
	for (index = 0; index < 3; index++) {
		storage = draw->storage[2U + index * 2U];
		source = storage->view->buffer->address;
		for (byte = 0; byte < 8; byte++)
			assert(source[byte] == (uint8_t)(event->pipeline->programs[index]->code[0] >> (byte * 8U)));
	}

	/* Packed fetch exactly copies the six logical vec2 vertices and every later padded byte stays initialized zero. */
	storage = draw->storage[9];
	assert(storage->bytes == 48);
	source = storage->view->buffer->address;
	for (index = 0; index < 48; index++)
		assert(source[index] == index + 1U);
	for (index = 48; index < 4096; index++)
		assert(source[index] == 0);
	storage = draw->storage[10];
	assert(draw->shader == storage->view->address);
	record = storage->view->buffer->address;
	assert(record[40] == 10U && record[41] == 0x22U && record[44] == 8U && record[48] == 5U);

	/* Later coherent raster/fetch mutation leaves the existing independent GPU input snapshot unchanged. */
	error = bcm2711_vulkan_resource_backing(event->vertices[0], 0, 48, &view, &address, &cpu);
	assert(error == 0);
	memset(cpu, 0x93, 48);
	assert(source[0] == 1 && source[47] == 48);
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	assert(error == 0);
	memset(cpu, 0x24, 512);
	assert(((uint8_t *)draw->storage[0]->view->buffer->address)[0] == 0x75);
	used = allocations;
	error = bcm2711_vulkan_native_draw_release(&draw, false);
	assert(error == EBUSY && draw != NULL && draw->bytes == owned && allocations == used);
	error = bcm2711_vulkan_native_draw_release(&draw, true);
	assert(error == 0 && draw == NULL && allocations == baseline);

	/* Early and late ordinary OOM retire complete mapped prefixes, including an independently copied scalar stream. */
	for (index = 1; index <= 4; index++) {
		fail_after = index * 10U;
		available = 1024U * 1024U;
		error = bcm2711_vulkan_native_draw_create(space, event, &available, &draw);
		assert(error == ENOMEM && draw == NULL && allocations == baseline && available == 1024U * 1024U);
		assert(fail_after == 0);
	}

	/* An insufficient whole-job budget refuses after a valid allocation prefix and preserves the caller's exact budget. */
	available = 4096;
	error = bcm2711_vulkan_native_draw_create(space, event, &available, &draw);
	assert(error == ENOMEM && draw == NULL && available == 4096 && allocations == baseline);

	/* Complete synthetic prepared copies exercise firstVertex rebasing and late logical fetch refusal without changing the real primary. */
	changed = kern_calloc(1, sizeof(*changed));
	assert(changed != NULL);
	*changed = *event;
	changed->draw[0] = 3;
	changed->draw[2] = 3;
	changed->scissor.offset.x = 2;
	changed->scissor.offset.y = 1;
	changed->scissor.extent.width = 4;
	changed->scissor.extent.height = 3;
	error = bcm2711_vulkan_resource_backing(event->vertices[0], 0, 48, &view, &address, &cpu);
	assert(error == 0);
	for (index = 0; index < 48; index++)
		((uint8_t *)cpu)[index] = (uint8_t)(index + 1);
	available = 1024U * 1024U;
	error = bcm2711_vulkan_native_draw_create(space, changed, &available, &draw);
	assert(error == 0 && draw->vertices == 3 && draw->storage[9]->bytes == 24);
	assert(draw->bin[16] == 2 && draw->bin[18] == 1 && draw->bin[20] == 4 && draw->bin[22] == 3);
	source = draw->storage[9]->view->buffer->address;
	for (index = 0; index < 24; index++)
		assert(source[index] == index + 25U);

	/* The numerical lowering also preserves an entirely off-drawable empty scissor without wrapped coordinates. */
	changed->scissor.offset.x = 4096;
	error = bcm2711_vulkan_native_bin_prepare(changed, draw);
	assert(error == 0 && draw->bin[16] == 0 && draw->bin[20] == 0 && draw->bin[22] == 3);
	changed->scissor.offset.x = 2;
	error = bcm2711_vulkan_native_draw_release(&draw, true);
	assert(error == 0 && draw == NULL && allocations == baseline + 1);

	/* A valid narrower float format fills its absent shader component with zero instead of the next source word. */
	pipeline = kern_calloc(1, sizeof(*pipeline));
	assert(pipeline != NULL);
	*pipeline = *event->pipeline;
	pipeline->attributes[0].format = VK_FORMAT_R32_SFLOAT;
	changed->pipeline = pipeline;
	available = 1024U * 1024U;
	error = bcm2711_vulkan_native_draw_create(space, changed, &available, &draw);
	assert(error == 0 && draw->attributes == 1 && draw->storage[9]->bytes == 24);
	source = draw->storage[9]->view->buffer->address;
	for (index = 0; index < 3; index++) {
		for (byte = 0; byte < 4; byte++) {
			assert(source[index * 8U + byte] == index * 8U + byte + 25U);
			assert(source[index * 8U + byte + 4U] == 0);
		}
	}

	/* Synthetic pipeline metadata owns no real pipeline reference; only the upload root is retired here. */
	error = bcm2711_vulkan_native_draw_release(&draw, true);
	assert(error == 0 && draw == NULL && allocations == baseline + 2);

	/* Synthetic zero-input metadata verifies the 4.2 mandatory unused CS/VS fetch; actual QPU execution is not simulated. */
	programs = kern_calloc(2, sizeof(*programs));
	assert(programs != NULL);
	for (index = 0; index < 2; index++) {
		programs[index] = *event->pipeline->programs[index];
		programs[index].input_count = 0;
		pipeline->programs[index] = &programs[index];
	}

	/* Defaults already belong to the native root, so the dummy needs no extra fetch mapping. */
	pipeline->attribute_count = 0;
	available = 1024U * 1024U;
	error = bcm2711_vulkan_native_draw_create(space, changed, &available, &draw);
	assert(error == 0 && draw->attributes == 1 && draw->count == 10);
	record = draw->storage[9]->view->buffer->address;
	assert(record[40] == 9U && record[41] == 0x11U && record[44] == 0);
	address = draw->storage[8]->view->address;
	for (byte = 0; byte < 4; byte++)
		assert(record[36U + byte] == (uint8_t)(address >> (byte * 8U)));
	error = bcm2711_vulkan_native_draw_release(&draw, true);
	assert(error == 0 && draw == NULL && allocations == baseline + 3);
	kern_free(programs);
	kern_free(pipeline);
	changed->pipeline = event->pipeline;
	changed->draw[0] = 4;
	available = 1024U * 1024U;
	error = bcm2711_vulkan_native_draw_create(space, changed, &available, &draw);
	assert(error == EINVAL && draw == NULL && allocations == baseline + 1 && available == 1024U * 1024U);
	kern_free(changed);
	puts("WS141 real compiled/native mapped draw/TMU-fetch snapshots/budget/uncertain retirement: PASS");

	/* Succeeded: every complete or refused upload prefix retires without changing the pending prepared primary. */
	return;
}

/* Exercises whole mapped pass ownership with real prepared targets/draws and synthetic no-op/malformed event copies. */
static void
native_pass_test(
	struct bcm2711_vulkan_session *session,
	const struct bcm2711_vulkan_prepared_event *begin_event)
{
	struct bcm2711_vulkan_native_pass *pass;
	const struct bcm2711_vulkan_prepared_event *following;
	struct bcm2711_vulkan_prepared_event *events;
	struct bcm2711_vulkan_framebuffer *framebuffer;
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	struct bcm2711_v3d_space *space;
	void *cpu;
	uint32_t address;
	uint32_t references;
	uint32_t index;
	uint32_t byte;
	unsigned baseline;
	unsigned held;
	unsigned cleans;
	uint64_t available;
	uint64_t bytes;
	int error;

	/* The actual immutable primary supplies its typed framebuffer and independently bound coherent colour allocation. */
	space = &session->render->device->space;
	framebuffer = begin_event->pass->objects[1]->payload;
	image_view = framebuffer->view->payload;
	image = image_view->owner.parent->payload;
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	assert(error == 0 && image->bytes == 512U);
	references = view->references;
	baseline = allocations;
	memset(cpu, 0x6c, 512);
	available = 8U * 1024U * 1024U;
	cleans = native_cleans;
	error = bcm2711_vulkan_native_pass_create(space, begin_event, &available, &pass, &following);
	assert(error == 0 && pass != NULL && following == NULL);
	assert(pass->output == view && view->references == references + 1U);
	assert(pass->count == 9U && pass->draws == 1U && pass->first == pass->last && pass->last->next == NULL);
	bytes = pass->bytes;
	assert(bytes == 1646592U && available == 8U * 1024U * 1024U - bytes);
	assert(native_cleans == cleans + 20U && pass->job.kind == BCM2711_V3D_JOB_CL);
	assert(pass->job.command.cl.bin_end - pass->job.command.cl.bin_start == 130U);
	assert(pass->job.command.cl.render_end - pass->job.command.cl.render_start == 106U);
	assert(pass->job.command.cl.pool_bytes == 0x83000U && pass->job.command.cl.state_bytes == 4096U);
	assert(pass->job.command.cl.overflow_count == 4U && pass->job.command.cl.clean_output);
	assert(pass->state.output == address && pass->state.load == 1U && pass->state.store == 1U);
	assert(pass->load == VK_ATTACHMENT_LOAD_OP_CLEAR && pass->cpu == cpu);
	for (index = 0; index < BCM2711_NATIVE_BIN_BYTES; index++)
		assert(((uint8_t *)pass->storage[0]->view->buffer->address)[BCM2711_NATIVE_PASS_BIN_PREFIX + index] == pass->first->bin[index]);
	for (index = 0; index < 4U; index++) {
		assert(pass->job.command.cl.overflow[index].address == pass->storage[5U + index]->view->address);
		assert(pass->job.command.cl.overflow[index].bytes == 256U * 1024U);
	}

	/* A successfully prepared CLEAR pass still leaves every existing target sample untouched until actual execution. */
	for (byte = 0; byte < 512U; byte++)
		assert(((uint8_t *)cpu)[byte] == 0x6c);
	held = allocations;
	error = bcm2711_vulkan_native_pass_release(&pass, false);
	assert(error == EBUSY && pass != NULL && allocations == held && pass->bytes == bytes);
	assert(view->references == references + 1U && pass->first != NULL && pass->count == 9U);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && pass == NULL && allocations == baseline && view->references == references);

	/* Selected late allocation failures retire complete draw/list/overflow prefixes and restore the caller's exact aggregate budget. */
	for (index = 0; index < 3U; index++) {
		fail_after = 10U + index * 25U;
		available = 8U * 1024U * 1024U;
		error = bcm2711_vulkan_native_pass_create(space, begin_event, &available, &pass, &following);
		assert(error == ENOMEM && pass == NULL && following == NULL && fail_after == 0);
		assert(allocations == baseline && view->references == references && available == 8U * 1024U * 1024U);
	}

	/* A budget sufficient for all draw owners but insufficient for the pass pool rolls back all earlier independent mappings. */
	available = 64U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, begin_event, &available, &pass, &following);
	assert(error == ENOMEM && pass == NULL && following == NULL && allocations == baseline);
	assert(view->references == references && available == 64U * 1024U);

	/* Synthetic immutable event copies exercise no-op draws and missing-END refusal without mutating the pending real primary. */
	events = kern_calloc(3, sizeof(*events));
	assert(events != NULL);
	events[0] = *begin_event;
	events[1] = *begin_event->next;
	events[2] = *begin_event->next->next;
	events[0].next = &events[1];
	events[1].next = &events[2];
	events[1].draw[0] = 0;
	available = 8U * 1024U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, events, &available, &pass, &following);
	assert(error == 0 && pass->draws == 0 && pass->first == NULL && pass->count == 9U);
	assert(pass->job.command.cl.bin_end - pass->job.command.cl.bin_start == 14U);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && allocations == baseline + 1U && view->references == references);
	events[1].draw[0] = 6;
	events[1].draw[1] = 0;
	available = 8U * 1024U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, events, &available, &pass, &following);
	assert(error == 0 && pass->draws == 0 && pass->first == NULL);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && allocations == baseline + 1U && view->references == references);
	events[1].draw[1] = 1;
	events[1].next = NULL;
	available = 8U * 1024U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, events, &available, &pass, &following);
	assert(error == EINVAL && pass == NULL && following == NULL && available == 8U * 1024U * 1024U);
	assert(allocations == baseline + 1U && view->references == references);
	kern_free(events);

	/* Overflow admission refuses before acquiring another framebuffer reference or native owner. */
	view->references = 0xffffffffU;
	available = 8U * 1024U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, begin_event, &available, &pass, &following);
	assert(error == EOVERFLOW && pass == NULL && allocations == baseline);
	view->references = references;
	for (byte = 0; byte < 512U; byte++)
		assert(((uint8_t *)cpu)[byte] == 0x6c);
	puts("WS141 whole mapped native pass/CL-tile-overflow/output ownership/no-op draw/atomic rollback: PASS");

	/* Succeeded: every complete or refused whole-pass root retires without modifying the real pending primary or target samples. */
	return;
}

/* Tests real exact-rectangle clear and native-pass handoff against an explicit runner fixture, never physical GPU completion. */
static void
native_execute_test(
	struct bcm2711_vulkan_session *session,
	const struct bcm2711_vulkan_prepared_event *begin_event)
{
	struct execute_fixture *fixture;
	struct bcm2711_vulkan_native_pass *pass;
	struct bcm2711_v3d_job_result result;
	const struct bcm2711_vulkan_prepared_event *following;
	struct bcm2711_v3d_space *space;
	uint8_t *cpu;
	uint64_t available;
	uint32_t x;
	uint32_t y;
	uint32_t index;
	uint32_t component;
	uint8_t expected[4];
	unsigned baseline;
	unsigned calls;
	unsigned writes;
	unsigned reads;
	unsigned held;
	int error;

	/* Copied numerical clear/area metadata borrows the actual independently retained framebuffer and pending primary. */
	space = &session->render->device->space;
	baseline = allocations;
	fixture = kern_calloc(1, sizeof(*fixture));
	assert(fixture != NULL);
	fixture->record = *begin_event->pass;
	fixture->record.area.offset.x = 2;
	fixture->record.area.offset.y = 1;
	fixture->record.area.extent.width = 3;
	fixture->record.area.extent.height = 2;
	fixture->record.words[0] = 0x3e800000U;
	fixture->record.words[1] = 0x3f000000U;
	fixture->record.words[2] = 0x3f400000U;
	fixture->record.words[3] = 0x3f800000U;
	fixture->events[0] = *begin_event;
	fixture->events[1] = *begin_event->next;
	fixture->events[2] = *begin_event->next->next;
	for (index = 0; index < 3; index++)
		fixture->events[index].pass = &fixture->record;
	fixture->events[0].next = &fixture->events[1];
	fixture->events[1].next = &fixture->events[2];
	available = 8U * 1024U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, fixture->events, &available, &pass, &following);
	assert(error == 0 && pass->format == VK_FORMAT_R8G8B8A8_UNORM);
	cpu = pass->cpu;
	memset(cpu, 0x6c, 512);
	space->native->hardware.initialized = true;
	space->native->power.ready = true;
	calls = native_execute_calls;
	writes = native_write_barriers;
	reads = native_read_barriers;

	/* Faulted admission refuses before CPU clear, root execution publication or any mocked native handoff. */
	space->native->hardware.ready = false;
	error = bcm2711_vulkan_native_pass_run(pass, &result);
	assert(error == EIO && result.retired && !pass->executed && native_execute_calls == calls);
	for (index = 0; index < 512; index++)
		assert(cpu[index] == 0x6c);
	space->native->hardware.ready = true;
	pass->area.extent.width = 4096;
	error = bcm2711_vulkan_native_pass_run(pass, &result);
	assert(error == EINVAL && result.retired && !pass->executed && native_execute_calls == calls);
	pass->area.extent.width = 3;

	/* The explicit runner fixture observes one real executor handoff with separately counted publication and visibility barriers. */
	native_execute_error = 0;
	native_execute_retired = true;
	error = bcm2711_vulkan_native_pass_run(pass, &result);
	assert(error == 0 && result.retired && pass->executed && pass->retired);
	assert(native_execute_calls == calls + 1 && native_write_barriers == writes + 1 && native_read_barriers == reads + 1);
	expected[0] = 64;
	expected[1] = 128;
	expected[2] = 191;
	expected[3] = 255;
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 16U; x++) {
			for (component = 0; component < 4U; component++) {
				/* All surrounding samples and every byte outside the exact three-by-two rectangle survive unchanged. */
				index = y * 64U + x * 4U + component;
				if (x >= 2U && x < 5U && y >= 1U && y < 3U)
					assert(cpu[index] == expected[component]);
				else
					assert(cpu[index] == 0x6c);
			}
		}
	}

	/* Completed roots cannot replay CPU clear or native work, even if the coherent target changes afterward. */
	memset(cpu, 0x5d, 512);
	error = bcm2711_vulkan_native_pass_run(pass, &result);
	assert(error == EBUSY && result.retired && native_execute_calls == calls + 1);
	assert(cpu[1U * 64U + 2U * 4U] == 0x5d);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && allocations == baseline + 1);

	/* Loading skips CPU clear, while uncertain synthetic native completion retains every whole-pass owner and refuses replay. */
	available = 8U * 1024U * 1024U;
	error = bcm2711_vulkan_native_pass_create(space, fixture->events, &available, &pass, &following);
	assert(error == 0);
	pass->load = VK_ATTACHMENT_LOAD_OP_LOAD;
	cpu = pass->cpu;
	memset(cpu, 0x6c, 512);
	native_execute_error = ETIMEDOUT;
	native_execute_retired = false;
	held = allocations;
	error = bcm2711_vulkan_native_pass_run(pass, &result);
	assert(error == ETIMEDOUT && !result.retired && pass->executed && !pass->retired && allocations == held);
	assert(native_execute_calls == calls + 2 && native_read_barriers == reads + 1);
	for (index = 0; index < 512; index++)
		assert(cpu[index] == 0x6c);
	error = bcm2711_vulkan_native_pass_release(&pass, false);
	assert(error == EBUSY && pass != NULL && allocations == held && pass->first != NULL);
	error = bcm2711_vulkan_native_pass_run(pass, &result);
	assert(error == EBUSY && !result.retired && native_execute_calls == calls + 2);

	/* No host fixture DMA was launched; this true release supplies no evidence of a production global-reset retirement. */
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && allocations == baseline + 1);
	native_execute_error = 0;
	native_execute_retired = true;
	kern_free(fixture);
	assert(allocations == baseline);
	puts("WS141 real rectangular coherent clear/native-runner fixture handoff/single-use retirement: PASS");

	/* Succeeded: every complete or refused private executor root has retired within this explicit non-DMA host fixture. */
	return;
}

/* Verifies actual pending/native pass payload transfer survives synthetic callback retirement until explicit recovery. */
static void
native_job_test(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *command_object)
{
	struct bcm2711_vulkan_native_job *job;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *render;
	unsigned baseline;
	unsigned held;
	uint32_t references;
	uint32_t index;
	bool retired;
	int error;

	/* Future runtime binding is explicit in this private fixture, without publishing COMMAND/CAPSET/JOB operations. */
	render = session->render;
	controller = render->device;
	command = command_object->payload;
	assert(command->pending == 0 && render->vulkan == NULL);
	controller->space.native->hardware.initialized = true;
	controller->space.native->power.ready = true;
	render->vulkan = session;
	baseline = allocations;
	references = command_object->references;
	for (index = 1; index <= 3U; index++) {
		fail_after = index;
		error = bcm2711_vulkan_native_job_create(command_object, &job);
		assert(error == ENOMEM && job == NULL && allocations == baseline && command->pending == 0);
		assert(command_object->references == references);
	}

	/* The complete queued CPU payload retains the primary before any coherent source snapshot or native allocation. */
	error = bcm2711_vulkan_native_job_create(command_object, &job);
	assert(error == 0 && job != NULL && job->pass == NULL && command->pending == 1);
	native_execute_error = 0;
	native_execute_retired = true;
	error = bcm2711_vulkan_native_job_execute(controller, render, job, &retired);
	assert(error == 0 && retired && job->pass == NULL && job->executed && command->pending == 1);
	error = bcm2711_vulkan_native_job_execute(controller, render, job, &retired);
	assert(error == EBUSY && retired);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && allocations == baseline && command->pending == 0 && controller->quarantine == NULL);

	/* An explicit native-runner timeout keeps the complete current pass, pending primary and descriptor graph alive after false disposal. */
	error = bcm2711_vulkan_native_job_create(command_object, &job);
	assert(error == 0);
	native_execute_error = ETIMEDOUT;
	native_execute_retired = false;
	error = bcm2711_vulkan_native_job_execute(controller, render, job, &retired);
	assert(error == ETIMEDOUT && !retired && job->pass != NULL && command->pending == 1);
	held = allocations;
	error = bcm2711_vulkan_native_job_dispose(controller, job, false);
	assert(error == 0 && controller->quarantine == job && job->quarantined && allocations == held);
	assert(job->prepared != NULL && job->pass->first != NULL && job->pass->output != NULL);
	error = bcm2711_vulkan_native_job_dispose(controller, job, false);
	assert(error == 0 && controller->quarantine == job && job->next == NULL && allocations == held);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == EBUSY && controller->quarantine == job && command->pending == 1 && allocations == held);

	/* External session ownership and unavailable native reset admission each leave the entire quarantined graph untouched. */
	controller->sessions = 1;
	error = bcm2711_vulkan_native_jobs_recover(controller);
	assert(error == EBUSY && controller->quarantine == job && command->pending == 1 && allocations == held);
	controller->sessions = 0;
	controller->space.native->hardware.ready = false;
	error = bcm2711_vulkan_native_jobs_recover(controller);
	assert(error == EIO && controller->quarantine == job && command->pending == 1 && allocations == held);
	controller->space.native->hardware.ready = true;

	/* No physical fixture DMA ran; synthetic reset admission authorizes only this host owner-graph retirement, not production reset acceptance. */
	error = bcm2711_vulkan_native_jobs_recover(controller);
	assert(error == 0 && controller->quarantine == NULL && allocations == baseline && command->pending == 0);
	assert(command_object->references == references);
	native_execute_error = 0;
	native_execute_retired = true;
	render->vulkan = NULL;
	puts("WS141 whole pending native job/controller quarantine/descriptor charges/explicit recovery: PASS");

	/* Succeeded: callback disposal cannot discard the sole root of any uncertain native pass or pending primary. */
	return;
}

/* Checks actual compiled consumption/cloned input ownership and synthetic UBO intervals against real coherent resource backing. */
static void
uniform_test(
	struct bcm2711_vulkan_session *session,
	const struct bcm2711_vulkan_prepared_event *event)
{
	struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS];
	struct bcm2711_vulkan_uniform_words *words;
	struct bcm2711_vulkan_uniform_words *second;
	struct bcm2711_shader_binary *program;
	const struct bcm2711_shader_uniform *uniform;
	struct uniform_fixture *fixture;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_resource *buffer;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint8_t *bytes;
	uint32_t address;
	uint32_t stage;
	uint32_t index;
	uint32_t expected;
	uint32_t push_stage;
	unsigned baseline;
	int error;

	/* Borrowed numerical addresses exercise encoding only; this fixture grants no real GPU record storage or execution. */
	baseline = allocations;
	memset(bindings, 0, sizeof(bindings));
	bindings[0][0].texture = 0x100020U;
	bindings[0][0].sampler = 0x100040U;
	for (stage = 0; stage < 3; stage++) {
		program = event->pipeline->programs[stage];
		error = bcm2711_vulkan_uniform_create(event, stage, bindings, &words);
		assert(error == 0 && words != NULL && words->count == program->uniform_count);
		push_stage = 0;
		if (stage == BCM2711_SHADER_FRAGMENT)
			push_stage = 1;

		/* Known real viewport is width16/height8/depth0..1, independently expecting scale2048/1024/1/0. */
		for (index = 0; index < words->count; index++) {
			uniform = &program->uniforms[index];
			expected = 0;
			switch (uniform->kind) {
			case BCM2711_SHADER_CONSTANT:
				expected = uniform->bits;
				break;
			case BCM2711_SHADER_PUSH:
				expected = event->push[push_stage][uniform->offset / 4U];
				break;
			case BCM2711_SHADER_TEXTURE:
				expected = 0x100020U | uniform->bits;
				break;
			case BCM2711_SHADER_SAMPLER:
				expected = 0x100040U | uniform->bits;
				break;
			case BCM2711_SHADER_VIEWPORT_X:
				expected = 0x45000000U;
				break;
			case BCM2711_SHADER_VIEWPORT_Y:
				expected = 0x44800000U;
				break;
			case BCM2711_SHADER_VIEWPORT_Z:
				expected = 0x3f800000U;
				break;
			case BCM2711_SHADER_DEPTH_OFFSET:
				break;
			default:
				assert(0);
			}

			/* The whole actual emitted consumption order must match these independently expected values. */
			assert(words->words[index] == expected);
		}

		/* Each stage returns to the original ownership baseline before the next stream is assembled. */
		bcm2711_vulkan_uniform_release(words);
		assert(allocations == baseline);
	}

	/* Root and array OOM unwind the entire unpublished scalar owner. */
	for (index = 1; index <= 2; index++) {
		fail_after = index;
		error = bcm2711_vulkan_uniform_create(event, BCM2711_SHADER_FRAGMENT, bindings, &words);
		assert(error == ENOMEM && words == NULL && allocations == baseline);
	}

	/* A late sampled pointer refusal consumes no pending-primary references or scalar allocations. */
	error = bcm2711_vulkan_uniform_create(event, BCM2711_SHADER_FRAGMENT, NULL, &words);
	assert(error == EINVAL && words == NULL && allocations == baseline);
	bindings[0][0].texture++;
	error = bcm2711_vulkan_uniform_create(event, BCM2711_SHADER_FRAGMENT, bindings, &words);
	assert(error == EINVAL && words == NULL && allocations == baseline);

	/* Synthetic immutable metadata adds UBO consumption without rewriting the pending client's actual pipeline or clones. */
	fixture = kern_calloc(1, sizeof(*fixture));
	assert(fixture != NULL);
	fixture->event = *event;
	fixture->pipeline = *event->pipeline;
	fixture->layout = *(struct bcm2711_vulkan_pipeline_layout *)event->pipeline->owner.parent->payload;
	fixture->set = *(struct bcm2711_vulkan_set_layout *)fixture->layout.sets[0]->payload;
	fixture->program = *event->pipeline->programs[BCM2711_SHADER_VERTEX];
	fixture->event.pipeline = &fixture->pipeline;
	fixture->pipeline.owner.parent = &fixture->layout_object;
	fixture->layout_object.payload = &fixture->layout;
	fixture->layout.sets[0] = &fixture->set_object;
	fixture->set_object.payload = &fixture->set;
	fixture->set.bindings[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	fixture->program.uniforms = fixture->uniforms;
	fixture->program.uniform_count = 3;
	fixture->pipeline.programs[BCM2711_SHADER_VERTEX] = &fixture->program;
	fixture->uniforms[0].kind = BCM2711_SHADER_CONSTANT;
	fixture->uniforms[0].bits = 0x12345678U;
	fixture->uniforms[1].kind = BCM2711_SHADER_BLOCK;
	fixture->uniforms[2].kind = BCM2711_SHADER_BLOCK;
	fixture->uniforms[2].offset = 4;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, 101);
	assert(object != NULL);
	buffer = object->payload;
	memset(fixture->event.descriptors, 0, sizeof(fixture->event.descriptors));
	fixture->event.descriptors[0][0].buffer = object;
	fixture->event.descriptors[0][0].offset = 4;
	fixture->event.descriptors[0][0].bytes = 8;
	error = bcm2711_vulkan_resource_backing(buffer, 4, 8, &view, &address, &cpu);
	assert(error == 0);
	bytes = cpu;

	/* Distinct little-endian words prove exact binding+static offsets and copied results across later completed writes. */
	for (index = 0; index < 8; index++)
		bytes[index] = (uint8_t)(index + 1);
	error = bcm2711_vulkan_uniform_create(&fixture->event, BCM2711_SHADER_VERTEX, NULL, &words);
	assert(error == 0 && words->count == 3 && words->words[0] == 0x12345678U);
	assert(words->words[1] == 0x04030201U && words->words[2] == 0x08070605U);
	for (index = 0; index < 8; index++)
		bytes[index] = (uint8_t)(index + 17);
	error = bcm2711_vulkan_uniform_create(&fixture->event, BCM2711_SHADER_VERTEX, NULL, &second);
	assert(error == 0 && second->words[1] == 0x14131211U && second->words[2] == 0x18171615U);
	assert(words->words[1] == 0x04030201U && words->words[2] == 0x08070605U);
	bcm2711_vulkan_uniform_release(words);
	bcm2711_vulkan_uniform_release(second);

	/* Exact descriptor bytes, logical buffer end and quarantined native backing refuse the complete scalar prefix. */
	fixture->event.descriptors[0][0].bytes = 7;
	error = bcm2711_vulkan_uniform_create(&fixture->event, BCM2711_SHADER_VERTEX, NULL, &words);
	assert(error == EINVAL && words == NULL && allocations == baseline + 1);
	fixture->event.descriptors[0][0].bytes = 8;
	fixture->event.descriptors[0][0].offset = 64;
	error = bcm2711_vulkan_uniform_create(&fixture->event, BCM2711_SHADER_VERTEX, NULL, &words);
	assert(error == EINVAL && words == NULL && allocations == baseline + 1);
	fixture->event.descriptors[0][0].offset = 4;
	view->quarantined = true;
	error = bcm2711_vulkan_uniform_create(&fixture->event, BCM2711_SHADER_VERTEX, NULL, &words);
	assert(error == EIO && words == NULL && allocations == baseline + 1);
	view->quarantined = false;
	fixture->event.used[0] = 0;
	error = bcm2711_vulkan_uniform_create(&fixture->event, BCM2711_SHADER_VERTEX, NULL, &words);
	assert(error == EINVAL && words == NULL && allocations == baseline + 1);
	kern_free(fixture);
	assert(allocations == baseline);
	puts("WS141 native scalar uniforms/real compiled order/copied UBO FIFO reads/late atomic refusal: PASS");

	/* Succeeded: no independent scalar owner or synthetic metadata remains in the real pending primary. */
	return;
}

/* Exercises actual prepared ownership, pending guards and complete rollback without launching native GPU work. */
static void
prepared_test(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *command_object)
{
	struct bcm2711_vulkan_prepared *prepared;
	struct bcm2711_vulkan_prepared *second;
	struct bcm2711_vulkan_prepared_event *event;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_object *set_object;
	struct bcm2711_vulkan_object *sampler;
	struct bcm2711_vulkan_object *view;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[8192];
	unsigned baseline;
	uint32_t index;
	uint32_t command_references;
	uint32_t view_references;
	uint32_t sampler_references;
	int error;

	/* Every actual prepared allocation stage must retire its earlier prefix and acquired primary edge on ordinary OOM. */
	command = command_object->payload;
	baseline = allocations;
	command_references = command_object->references;
	set_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 163);
	assert(set_object != NULL);
	set = set_object->payload;
	view = set->bindings[0].view;
	sampler = set->bindings[0].sampler;
	view_references = view->references;
	sampler_references = sampler->references;
	for (index = 1; index <= 6; index++) {
		fail_after = index;
		error = bcm2711_vulkan_prepared_create(command_object, &prepared);
		assert(error == ENOMEM && prepared == NULL && allocations == baseline);
		assert(command_object->references == command_references && command->pending == 0 && set->pending == 0);
		assert(view->references == view_references && sampler->references == sampler_references);
	}

	/* A sampler retain overflow must release the newly retained view and all complete prior preparation nodes. */
	sampler->references = 0xffffffffU;
	error = bcm2711_vulkan_prepared_create(command_object, &prepared);
	assert(error == EOVERFLOW && prepared == NULL && allocations == baseline && view->references == view_references);
	sampler->references = sampler_references;
	set->pending = 0xffffffffU;
	error = bcm2711_vulkan_prepared_create(command_object, &prepared);
	assert(error == EOVERFLOW && prepared == NULL && allocations == baseline && command->pending == 0);
	set->pending = 0;

	/* Simultaneous-use metadata permits checking primary counter overflow without bypassing admission policy. */
	command->flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	command->pending = 0xffffffffU;
	error = bcm2711_vulkan_prepared_create(command_object, &prepared);
	assert(error == EOVERFLOW && prepared == NULL && allocations == baseline && set->pending == 0);
	command->pending = 0;
	error = bcm2711_vulkan_prepared_create(command_object, &prepared);
	assert(error == 0 && prepared != NULL);
	error = bcm2711_vulkan_prepared_create(command_object, &second);
	assert(error == 0 && second != NULL && command->pending == 2 && set->pending == 2);
	error = bcm2711_vulkan_prepared_release(second, true);
	assert(error == 0 && command->pending == 1 && set->pending == 1);
	error = bcm2711_vulkan_prepared_release(prepared, true);
	assert(error == 0 && command->pending == 0 && set->pending == 0 && allocations == baseline);
	command->flags = 0;
	native_job_test(session, command_object);

	/* A complete prepared primary acquires one distinct set charge and one independent snapshot of each consumed view/sampler. */
	error = bcm2711_vulkan_prepared_create(command_object, &prepared);
	assert(error == 0 && prepared != NULL && prepared->pending && command->pending == 1 && set->pending == 1);
	assert(command_object->references == command_references + 1 && view->references == view_references + 1 && sampler->references == sampler_references + 1);
	event = prepared->first;
	assert(event->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS && event->pass != NULL);
	event = event->next;
	assert(event->opcode == GPU_OP_CMD_DRAW && event->draw[0] == 6 && event->used[0] == 1);
	assert(event->descriptors[0][0].view == view && event->descriptors[0][0].sampler == sampler);
	assert(event->push[0][0] == 0x3f000000U && event->vertices[0]->bytes == 48);
	assert(event->next->opcode == GPU_OP_CMD_END_RENDER_PASS && event->next->next == NULL);
	native_draw_test(session, event);
	native_pass_test(session, prepared->first);
	native_execute_test(session, prepared->first);
	uniform_test(session, event);
	error = bcm2711_vulkan_prepared_create(command_object, &second);
	assert(error == EBUSY && second == NULL && command->pending == 1 && set->pending == 1);
	error = bcm2711_vulkan_prepared_release(prepared, false);
	assert(error == EBUSY && command->pending == 1 && set->pending == 1 && event->descriptors[0][0].view == view);

	/* Actual ordinary descriptor update refuses a pending set and leaves its independently cloned sampled inputs unchanged. */
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 163, 161, 104);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY && set->bindings[0].view == view && event->descriptors[0][0].view == view);

	/* Actual command reset refuses mutation while prepared work owns the frozen primary nodes. */
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u32(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE && command->first != NULL);

	/* Pending descriptor free validates the whole two-member vector before withdrawing even its idle first selection. */
	begin(&writer, wire, sizeof(wire), GPU_OP_FREE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 160);
	vulkan_write_u32(&writer, 2);
	vulkan_write_u64(&writer, 2);
	vulkan_write_u64(&writer, 164);
	vulkan_write_u64(&writer, 163);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY);
	assert(bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 164) != NULL && set_object->published);

	/* Pool reset and destroy also reject the complete operation before withdrawing any idle or pending set identity. */
	for (index = 0; index < 2; index++) {
		if (index == 0) {
			begin(&writer, wire, sizeof(wire), GPU_OP_RESET_DESCRIPTOR_POOL, 1);
		} else {
			begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_DESCRIPTOR_POOL, 1);
		}

		/* Both lifecycle variants address the same complete device/pool record. */
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 160);
		if (index == 0) {
			vulkan_write_u32(&writer, 0);
		} else {
			vulkan_write_u64(&writer, 0);
		}

		/* Execute the actual pending guard and verify that no public identity prefix changed. */
		error = execute(session, &writer, &reader);
		assert(error == EBUSY && set_object->published);
		assert(bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_POOL, 160) != NULL);
		assert(bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 164) != NULL);
	}

	/* Before native launch the caller can prove retirement directly, release all snapshots, and restore normal mutation admission. */
	error = bcm2711_vulkan_prepared_release(prepared, true);
	assert(error == 0 && command->pending == 0 && set->pending == 0 && allocations == baseline);
	assert(command_object->references == command_references && view->references == view_references && sampler->references == sampler_references);
	puts("WS141 Vulkan actual prepared CPU graph/descriptor snapshots/pending free-reset guards/uncertainty retain/OOM rollback: PASS");

	/* Succeeded: no actual DMA was launched and every prepared owner returned to the unchanged recording baseline. */
	return;
}

/* Verifies complete state admission and explicitly injected fixture faults against actual native draw walking. */
static void
draw_test(
	struct bcm2711_vulkan_command_buffer *command)
{
	struct bcm2711_vulkan_command_node *node;
	struct bcm2711_vulkan_record *draw;
	struct bcm2711_vulkan_record *push;
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_pipeline_layout combined;
	struct bcm2711_vulkan_pipeline_layout separate;
	struct bcm2711_vulkan_command_buffer prefixed;
	struct bcm2711_vulkan_pipeline *pipeline;
	struct bcm2711_vulkan_record prefix[2];
	struct bcm2711_vulkan_object layout_object;
	struct bcm2711_vulkan_object *set_object;
	uint32_t observation[4];
	unsigned baseline;
	int error;

	/* The callback observes actual recorded draw state; no native launch or GPU completion is simulated. */
	baseline = allocations;
	memset(observation, 0, sizeof(observation));
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == 0 && observation[0] == 3 && observation[2] == 1 && allocations == baseline);
	fail_after = 1;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == ENOMEM && observation[0] == 3 && allocations == baseline);
	memset(observation, 0, sizeof(observation));
	observation[1] = GPU_OP_CMD_DRAW;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == ENOMEM && observation[0] == 2 && allocations == baseline);

	/* The complete validation walk must reject a late fetch error before even the pass-begin callback can prepare a prefix. */
	draw = NULL;
	push = NULL;
	set = NULL;
	pipeline = NULL;
	set_object = NULL;
	for (node = command->first; node != NULL; node = node->next) {
		if (((struct bcm2711_vulkan_record *)node)->opcode == GPU_OP_CMD_DRAW)
			draw = (struct bcm2711_vulkan_record *)node;
		if (((struct bcm2711_vulkan_record *)node)->opcode == GPU_OP_CMD_PUSH_CONSTANTS)
			push = (struct bcm2711_vulkan_record *)node;
		if (((struct bcm2711_vulkan_record *)node)->opcode == GPU_OP_CMD_BIND_DESCRIPTOR_SETS) {
			set_object = ((struct bcm2711_vulkan_record *)node)->objects[1];
			set = set_object->payload;
		}

		/* Actual pipeline ownership supplies the exact compatibility definition for the synthetic binding-prefix fixture. */
		if (((struct bcm2711_vulkan_record *)node)->opcode == GPU_OP_CMD_BIND_PIPELINE)
			pipeline = ((struct bcm2711_vulkan_record *)node)->objects[0]->payload;
	}

	/* Seven vertices exceed the logical 48-byte buffer even though its rounded memory binding has more storage. */
	assert(draw != NULL && push != NULL && set != NULL);
	memset(observation, 0, sizeof(observation));
	draw->words[0] = 7;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == EINVAL && observation[0] == 0 && allocations == baseline);
	draw->words[0] = 6;
	draw->words[2] = 1;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == EINVAL && observation[0] == 0);
	draw->words[2] = 0;

	/* A fault-injected push to the other stage cannot define the vertex shader's previously unset words. */
	push->stages = VK_SHADER_STAGE_FRAGMENT_BIT;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == EINVAL && observation[0] == 0);
	push->stages = VK_SHADER_STAGE_VERTEX_BIT;

	/* Distinct image objects aliasing the attachment's storage must not bypass the unsupported feedback guard. */
	image_view = set->bindings[0].view->payload;
	image = image_view->owner.parent->payload;
	assert(image->offset == 2048);
	image->offset = 0;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == ENOTSUP && observation[0] == 0);
	image->offset = 2048;
	error = bcm2711_vulkan_draw_walk(command, draw_observe, observation);
	assert(error == 0 && observation[0] == 3 && allocations == baseline);

	/* Synthetic immutable binding prefixes use real set owners while exercising production disturbance beyond Keiland's single-set stream. */
	assert(pipeline != NULL && set_object != NULL);
	combined = *(struct bcm2711_vulkan_pipeline_layout *)pipeline->owner.parent->payload;
	combined.count = 2;
	combined.sets[1] = combined.sets[0];
	memset(&layout_object, 0, sizeof(layout_object));
	layout_object.kind = I915_VK_OBJ_PIPELINE_LAYOUT;
	layout_object.payload = &combined;
	memset(prefix, 0, sizeof(prefix));
	prefix[0].opcode = GPU_OP_CMD_BIND_DESCRIPTOR_SETS;
	prefix[0].count = 2;
	prefix[0].objects[0] = &layout_object;
	prefix[0].objects[1] = set_object;
	prefix[0].objects[2] = set_object;
	prefix[0].generations[0] = set->generation;
	prefix[0].generations[1] = set->generation;
	prefix[0].node.next = command->first;
	prefixed = *command;
	prefixed.first = &prefix[0].node;
	memset(observation, 0, sizeof(observation));
	observation[3] = 1;
	error = bcm2711_vulkan_draw_walk(&prefixed, draw_observe, observation);
	assert(error == 0 && observation[0] == 3 && allocations == baseline);

	/* Replacing set zero using an incompatible push-range layout disturbs set one; rebinding real set zero does not resurrect it. */
	prefix[1] = prefix[0];
	prefix[1].count = 1;
	prefix[1].objects[0] = pipeline->owner.parent;
	prefix[1].node.next = command->first;
	prefix[0].node.next = &prefix[1].node;
	combined.ranges[0].size = 16;
	memset(observation, 0, sizeof(observation));
	observation[3] = 2;
	error = bcm2711_vulkan_draw_walk(&prefixed, draw_observe, observation);
	assert(error == 0 && observation[0] == 3 && allocations == baseline);

	/* Equal per-word stage permissions do not make combined and separate exact push ranges compatible. */
	memset(&combined, 0, sizeof(combined));
	memset(&separate, 0, sizeof(separate));
	combined.range_count = 1;
	combined.ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	combined.ranges[0].size = 16;
	separate.range_count = 2;
	separate.ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	separate.ranges[0].size = 16;
	separate.ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	separate.ranges[1].size = 16;
	error = bcm2711_vulkan_layout_push_compatible(&combined, &separate);
	assert(error == EINVAL);
	combined = separate;
	combined.ranges[0] = separate.ranges[1];
	combined.ranges[1] = separate.ranges[0];
	error = bcm2711_vulkan_layout_push_compatible(&combined, &separate);
	assert(error == 0);
	puts("WS141 Vulkan actual ordered draw state/full fetch/push compatibility/no prefix preparation on failure: PASS");

	/* Succeeded: all explicit fixture mutations were restored and the actual recording remains executable. */
	return;
}

/* Checks real public barrier encoding, independently pending ownership and atomic FIFO layout publication. */
static void
barrier_test(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *primary)
{
	struct bcm2711_vulkan_command_buffer *original;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *image_objects[2];
	struct bcm2711_vulkan_framebuffer *framebuffer;
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *images[2];
	struct bcm2711_vulkan_barrier *barrier;
	struct bcm2711_vulkan_native_job *job;
	struct vulkan_object pool;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[4096];
	uint64_t image_ids[2];
	VkImageLayout layouts[2];
	uint32_t references[2];
	unsigned baseline;
	unsigned writes;
	unsigned calls;
	uint32_t index;
	bool retired;
	int error;

	/* Actual immutable framebuffer and texture owners supply two independently bound coherent image resources. */
	original = primary->payload;
	framebuffer = ((struct bcm2711_vulkan_record *)original->first)->objects[1]->payload;
	image_view = framebuffer->view->payload;
	image_objects[0] = image_view->owner.parent;
	image_objects[1] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, 180);
	assert(image_objects[1] != NULL && image_objects[0] != image_objects[1]);

	/* An explicit prior-layout fixture makes the late mismatch test distinct from Vulkan's legitimate UNDEFINED discard transition. */
	images[1] = image_objects[1]->payload;
	images[1]->layout = VK_IMAGE_LAYOUT_GENERAL;
	for (index = 0; index < 2; index++) {
		images[index] = image_objects[index]->payload;
		image_ids[index] = image_objects[index]->identity;
		layouts[index] = images[index]->layout;
		references[index] = image_objects[index]->references;
	}

	/* A separate actual primary belongs to the existing reset-enabled device command pool. */
	memset(&pool, 0, sizeof(pool));
	pool.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 152);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 152);
	assert(object != NULL);
	command = object->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	/* The real wrapper copies global, buffer and two image dependencies into one bounded immutable node. */
	begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 152);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
	writer.bytes -= 8;
	ws141_client_encode_barrier(&writer, 152, 170, image_ids, layouts);
	vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 152);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	barrier = (struct bcm2711_vulkan_barrier *)command->first;
	assert(barrier->count == 4 && command->first == command->last);
	assert(barrier->entries[1].bytes == VK_WHOLE_SIZE && barrier->entries[2].object == image_objects[0]);
	for (index = 0; index < 2; index++)
		assert(image_objects[index]->references == references[index] + 1);

	/* A FIFO mismatch on the last image refuses atomically without layout publication or native launch. */
	baseline = allocations;
	writes = native_write_barriers;
	calls = native_execute_calls;
	session->render->vulkan = session;
	error = bcm2711_vulkan_native_job_create(object, &job);
	assert(error == 0 && command->pending == 1);
	images[1]->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	error = bcm2711_vulkan_native_job_execute(session->render->device, session->render, job, &retired);
	assert(error == EINVAL && retired && images[0]->layout == layouts[0]);
	assert(native_write_barriers == writes && native_execute_calls == calls);
	images[1]->layout = layouts[1];
	error = bcm2711_vulkan_native_job_dispose(session->render->device, job, true);
	assert(error == 0 && command->pending == 0 && allocations == baseline);

	/* Complete successful serial execution changes both exact layouts once while retaining the primary until disposal. */
	error = bcm2711_vulkan_native_job_create(object, &job);
	assert(error == 0);
	error = bcm2711_vulkan_native_job_execute(session->render->device, session->render, job, &retired);
	assert(error == 0 && retired && command->pending == 1);
	assert(images[0]->layout == VK_IMAGE_LAYOUT_GENERAL && images[1]->layout == VK_IMAGE_LAYOUT_GENERAL);
	assert(native_write_barriers == writes + 1 && native_execute_calls == calls);
	error = bcm2711_vulkan_native_job_dispose(session->render->device, job, true);
	assert(error == 0 && command->pending == 0 && allocations == baseline);
	session->render->vulkan = NULL;

	/* Two overlapping layout transitions of one implemented subresource retain no partial recording owner. */
	begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 152);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	image_ids[1] = image_ids[0];
	layouts[0] = VK_IMAGE_LAYOUT_GENERAL;
	layouts[1] = VK_IMAGE_LAYOUT_GENERAL;
	begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
	writer.bytes -= 8;
	ws141_client_encode_barrier(&writer, 152, 170, image_ids, layouts);
	vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 152);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) != VK_SUCCESS);
	assert(command->state == BCM2711_VULKAN_COMMAND_INVALID && command->first == NULL);
	for (index = 0; index < 2; index++) {
		assert(image_objects[index]->references == references[index]);
		images[index]->layout = layouts[index];
	}

	/* Removing this fixture's primary leaves the real graphics recording and every original typed resource owner intact. */
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, 152);
	assert(error == 0 && original->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
	puts("WS141 actual public barrier recording/pending typed owners/atomic FIFO layouts/duplicate refusal: PASS");
}

/* Verifies actual public transfer framing, owned GPU geometry/inputs, alias exclusion and pending retirement without a pixel-rendering mock. */
static void
transfer_test(
	struct bcm2711_vulkan_session *session)
{
	struct bcm2711_vulkan_object *primary;
	struct bcm2711_vulkan_object *image_object;
	struct bcm2711_vulkan_object *source_object;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_resource *source;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_transfer *transfer;
	struct bcm2711_vulkan_transfer *fixture;
	struct bcm2711_vulkan_native_pass *pass;
	struct bcm2711_vulkan_native_job *job;
	struct bcm2711_render_device *controller;
	struct bcm2711_v3d_view *view;
	struct vulkan_object pool;
	VkImageCreateInfo info;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	VkImageCopy copies[2];
	VkImageBlit blit;
	VkImageLayout saved_layout;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[4096];
	uint8_t saved[16384];
	uint32_t *vertices;
	void *cpu;
	uint64_t available;
	uint32_t address;
	uint32_t calls;
	uint32_t index;
	uint32_t references;
	uint32_t expected[2];
	float oracle[2];
	unsigned baseline;
	bool retired;
	int error;

	/* An actual transfer-only BGRA image has a disjoint logical binding and non-power-of-two dimensions. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_B8G8R8A8_UNORM;
	info.extent.width = 13;
	info.extent.height = 7;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, &info);
	error = input_created(session, &writer, 195);
	assert(error == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_IMAGE_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 195);
	vulkan_write_u64(&writer, 102);
	vulkan_write_u64(&writer, 4096);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	image_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, 195);
	source_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, 180);
	assert(image_object != NULL && source_object != NULL);
	image = image_object->payload;
	source = source_object->payload;
	saved_layout = source->layout;
	source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	assert(error == 0 && image->bytes <= sizeof(saved));
	memcpy(saved, cpu, image->bytes);
	references = source_object->references;
	controller = session->render->device;

	/* A distinct actual primary retains the same reset-enabled device pool without affecting ordinary graphics work. */
	memset(&pool, 0, sizeof(pool));
	pool.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 154);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	primary = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 154);
	assert(primary != NULL);
	command = primary->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	memset(copies, 0, sizeof(copies));
	for (index = 0; index < 2; index++) {
		copies[index].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copies[index].srcSubresource.layerCount = 1;
		copies[index].dstSubresource = copies[index].srcSubresource;
		copies[index].srcOffset.x = 1;
		copies[index].srcOffset.y = 1 + index * 3U;
		copies[index].dstOffset.x = 3;
		copies[index].dstOffset.y = 1 + index * 3U;
		copies[index].extent.width = 6;
		copies[index].extent.height = 2;
		copies[index].extent.depth = 1;
	}

	/* A bad trailing region is completely consumed without either typed edge; a fresh recording retains both valid rectangles. */
	for (index = 0; index < 2; index++) {
		copies[1].dstSubresource.mipLevel = 1;
		if (index != 0)
			copies[1].dstSubresource.mipLevel = 0;
		begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
		vulkan_write_u64(&writer, 154);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
		writer.bytes -= 8;
		ws141_client_encode_transfer(&writer, 154, 180, 195, source->layout, image->layout, copies, NULL, 2, VK_FILTER_NEAREST);
		vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
		vulkan_write_u32(&writer, 1);
		vulkan_write_u64(&writer, 154);
		error = execute(session, &writer, &reader);
		assert(error == 0);
		if (index == 0) {
			assert(vulkan_read_u32(&reader) != VK_SUCCESS && command->first == NULL && command->recording_error == EINVAL);
			assert(source_object->references == references && image_object->references == 1);
		} else {
			assert(vulkan_read_u32(&reader) == VK_SUCCESS);
		}
	}

	/* The fresh complete actual primary retains both typed images and all copied region metadata. */
	transfer = (struct bcm2711_vulkan_transfer *)command->first;
	assert(transfer->record.count == 2 && transfer->record.opcode == GPU_OP_CMD_COPY_IMAGE && transfer->regions[1].source[1] == 4);
	assert(source_object->references == references + 1 && image_object->references == 2);
	session->render->vulkan = session;
	baseline = allocations;
	error = bcm2711_vulkan_native_job_create(primary, &job);
	assert(error == 0 && command->pending == 1);
	available = 256ULL * 1024U * 1024U;
	error = bcm2711_vulkan_native_image_create(&controller->space, transfer, 0, &available, &pass);
	assert(error == 0 && pass != NULL && pass->draws == 1 && pass->first->vertices == 6 && pass->first->count == 11);
	assert(pass->state.load == 1 && pass->state.store == 1 && pass->area.offset.x == 3 && pass->area.extent.width == 6);
	vertices = pass->first->storage[9]->view->buffer->address;
	assert(vertices[0] == 0xbf800000U && vertices[3] == 0x3f800000U && vertices[4] == 0x3d800000U && vertices[5] == 0x3e000000U);
	assert(memcmp(cpu, saved, image->bytes) == 0);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && pass == NULL);
	calls = native_execute_calls;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == 0 && retired && native_execute_calls == calls + 2 && command->pending == 1 && job->pass == NULL);
	assert(memcmp(cpu, saved, image->bytes) == 0);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && allocations == baseline && command->pending == 0);

	/* Re-recording a real linear blit preserves independent source-X and destination-Y reversals. */
	memset(&blit, 0, sizeof(blit));
	blit.srcSubresource = copies[0].srcSubresource;
	blit.dstSubresource = copies[0].dstSubresource;
	blit.srcOffsets[0].x = 8;
	blit.srcOffsets[0].y = 1;
	blit.srcOffsets[1].x = 2;
	blit.srcOffsets[1].y = 7;
	blit.srcOffsets[1].z = 1;
	blit.dstOffsets[0].x = 1;
	blit.dstOffsets[0].y = 6;
	blit.dstOffsets[1].x = 11;
	blit.dstOffsets[1].y = 1;
	blit.dstOffsets[1].z = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 154);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
	writer.bytes -= 8;
	ws141_client_encode_transfer(&writer, 154, 180, 195, source->layout, image->layout, NULL, &blit, 1, VK_FILTER_LINEAR);
	vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 154);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	transfer = (struct bcm2711_vulkan_transfer *)command->first;
	assert(transfer->filter == VK_FILTER_LINEAR && transfer->regions[0].source[0] == 8 && transfer->regions[0].destination[1] == 6);
	baseline = allocations;
	available = 256ULL * 1024U * 1024U;
	error = bcm2711_vulkan_native_image_create(&controller->space, transfer, 0, &available, &pass);
	assert(error == 0 && pass != NULL && pass->area.offset.y == 1 && pass->area.extent.height == 5);
	vertices = pass->first->storage[9]->view->buffer->address;
	assert(vertices[4] == 0x3f000000U && vertices[5] == 0x3f600000U && vertices[10] == 0x3e000000U);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && allocations == baseline && memcmp(cpu, saved, image->bytes) == 0);

	/* Native budget refusal unwinds a whole compiler/input prefix and preserves both logical output and caller budget. */
	available = 4096;
	error = bcm2711_vulkan_native_image_create(&controller->space, transfer, 0, &available, &pass);
	assert(error == ENOMEM && pass == NULL && available == 4096 && allocations == baseline && memcmp(cpu, saved, image->bytes) == 0);
	error = bcm2711_vulkan_native_job_create(primary, &job);
	assert(error == 0);
	native_execute_error = ETIMEDOUT;
	native_execute_retired = false;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == ETIMEDOUT && !retired && command->pending == 1 && job->pass != NULL && job->pass->first != NULL);
	error = bcm2711_vulkan_native_job_dispose(controller, job, false);
	assert(error == 0 && controller->quarantine == job);
	error = bcm2711_vulkan_native_jobs_recover(controller);
	assert(error == 0 && allocations == baseline && command->pending == 0 && controller->quarantine == NULL);
	native_execute_error = 0;
	native_execute_retired = true;
	session->render->vulkan = NULL;

	/* A separate copied validator fixture leaves the actual recorded command immutable. */
	fixture = kern_calloc(1, sizeof(*fixture));
	assert(fixture != NULL);
	memcpy(fixture, transfer, sizeof(*fixture));
	transfer = fixture;

	/* Exact overlap checks permit disjoint same-image rows and refuse any destination which can overwrite another region's source. */
	transfer->record.objects[1] = source_object;
	transfer->source_layout = VK_IMAGE_LAYOUT_GENERAL;
	transfer->destination_layout = VK_IMAGE_LAYOUT_GENERAL;
	transfer->record.opcode = GPU_OP_CMD_COPY_IMAGE;
	transfer->filter = VK_FILTER_NEAREST;
	transfer->regions[0].source[0] = 0;
	transfer->regions[0].source[1] = 0;
	transfer->regions[0].source[2] = 4;
	transfer->regions[0].source[3] = 2;
	transfer->regions[0].destination[0] = 8;
	transfer->regions[0].destination[1] = 4;
	transfer->regions[0].destination[2] = 12;
	transfer->regions[0].destination[3] = 6;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	assert(error == 0);
	transfer->regions[0].destination[0] = 2;
	transfer->regions[0].destination[1] = 1;
	transfer->regions[0].destination[2] = 6;
	transfer->regions[0].destination[3] = 3;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	assert(error == EINVAL);
	/* Constant-coordinate nearest and linear blits still sample real texels despite their zero-area source box. */
	transfer->record.opcode = GPU_OP_CMD_BLIT_IMAGE;
	transfer->regions[0].source[0] = 4;
	transfer->regions[0].source[1] = 2;
	transfer->regions[0].source[2] = 4;
	transfer->regions[0].source[3] = 2;
	transfer->regions[0].destination[0] = 3;
	transfer->regions[0].destination[1] = 1;
	transfer->regions[0].destination[2] = 4;
	transfer->regions[0].destination[3] = 2;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	assert(error == EINVAL);
	transfer->filter = VK_FILTER_LINEAR;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	assert(error == EINVAL);
	transfer->regions[0].destination[0] = 8;
	transfer->regions[0].destination[1] = 4;
	transfer->regions[0].destination[2] = 12;
	transfer->regions[0].destination[3] = 6;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	assert(error == 0);

	/* Actual BGRA source staging and normalized non-power-of-two UVs agree with independent host IEEE division. */
	transfer->record.objects[0] = image_object;
	transfer->record.objects[1] = source_object;
	transfer->source_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	transfer->destination_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	image->layout = transfer->source_layout;
	source->layout = transfer->destination_layout;
	transfer->regions[0].source[0] = 1;
	transfer->regions[0].source[1] = 1;
	transfer->regions[0].source[2] = 7;
	transfer->regions[0].source[3] = 5;
	transfer->regions[0].destination[0] = 2;
	transfer->regions[0].destination[1] = 2;
	transfer->regions[0].destination[2] = 8;
	transfer->regions[0].destination[3] = 6;
	oracle[0] = 1.0f / 13.0f;
	oracle[1] = 1.0f / 7.0f;
	memcpy(expected, oracle, sizeof(expected));
	available = 256ULL * 1024U * 1024U;
	error = bcm2711_vulkan_native_image_create(&controller->space, transfer, 0, &available, &pass);
	assert(error == 0 && pass != NULL);
	vertices = pass->first->storage[9]->view->buffer->address;
	assert(vertices[4] == expected[0] && vertices[5] == expected[1]);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0);
	kern_free(fixture);
	source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	/* Retiring only this fixture's primary and transfer-only image leaves every earlier actual graphics owner unchanged. */
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, 154);
	assert(error == 0 && source_object->references == references);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_IMAGE, 195);
	assert(error == 0);
	source->layout = saved_layout;
	puts("WS141 actual public copy/blit/internal SPIR-V/native texture quad/no CPU target writes/multi-region/alias retirement: PASS");

	/* Succeeded: native handoff is proved in software; the explicit mock runner supplies no physical pixel rendering evidence. */
	return;
}

/* Checks real public upload/readback byte order, partial image regions, both row strides and atomic FIFO refusal. */
static void
raster_copy_test(
	struct bcm2711_vulkan_session *session)
{
	struct bcm2711_vulkan_object *image_object;
	struct bcm2711_vulkan_object *buffer_object;
	struct bcm2711_vulkan_object *alias_object;
	struct bcm2711_vulkan_object *primary;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_native_job *job;
	struct bcm2711_vulkan_buffer_copy *copy;
	struct bcm2711_vulkan_buffer_copy *fixture;
	struct bcm2711_render_device *controller;
	struct bcm2711_v3d_view *view;
	struct vulkan_object pool;
	VkImageCreateInfo image_info;
	VkBufferCreateInfo buffer_info;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	VkBufferImageCopy uploads[2];
	VkBufferImageCopy readbacks[2];
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[4096];
	uint8_t expected_image[448];
	uint8_t expected_buffer[1536];
	void *image_cpu;
	void *buffer_cpu;
	uint32_t address;
	uint32_t index;
	uint32_t row;
	uint32_t calls;
	unsigned baseline;
	bool retired;
	int error;

	/* An actual non-power-of-two BGRA transfer image retains raw channel bytes in its independent coherent binding. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	image_info.extent.width = 13;
	image_info.extent.height = 7;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, &image_info);
	error = input_created(session, &writer, 195);
	assert(error == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_IMAGE_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 195);
	vulkan_write_u64(&writer, 102);
	vulkan_write_u64(&writer, 4096);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	image_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, 195);
	assert(image_object != NULL);
	image = image_object->payload;
	image->layout = VK_IMAGE_LAYOUT_GENERAL;
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &image_cpu);
	assert(error == 0 && image->pitch == 64 && image->bytes == sizeof(expected_image));

	/* One distinct actual buffer has enough logical bytes for upload data, packed readback and separately padded readback. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = sizeof(expected_buffer);
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_BUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferCreateInfo(&writer, &buffer_info);
	error = input_created(session, &writer, 196);
	assert(error == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_BUFFER_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 196);
	vulkan_write_u64(&writer, 102);
	vulkan_write_u64(&writer, 6144);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	buffer_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, 196);
	assert(buffer_object != NULL);
	error = bcm2711_vulkan_resource_backing(buffer_object->payload, 0, sizeof(expected_buffer), &view, &address, &buffer_cpu);
	assert(error == 0);

	/* A separate real primary uses the existing reset-enabled device pool and stores both transfer directions in one FIFO. */
	memset(&pool, 0, sizeof(pool));
	pool.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 156);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	primary = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 156);
	assert(primary != NULL);
	command = primary->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	memset(uploads, 0, sizeof(uploads));
	uploads[0].bufferOffset = 4;
	uploads[0].bufferRowLength = 11;
	uploads[0].bufferImageHeight = 5;
	uploads[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	uploads[0].imageSubresource.layerCount = 1;
	uploads[0].imageOffset.x = 3;
	uploads[0].imageOffset.y = 2;
	uploads[0].imageExtent.width = 7;
	uploads[0].imageExtent.height = 3;
	uploads[0].imageExtent.depth = 1;
	uploads[1].bufferOffset = 160;
	uploads[1].imageSubresource = uploads[0].imageSubresource;
	uploads[1].imageExtent.width = 2;
	uploads[1].imageExtent.height = 2;
	uploads[1].imageExtent.depth = 1;
	memset(readbacks, 0, sizeof(readbacks));
	readbacks[0].bufferOffset = 224;
	readbacks[0].imageSubresource = uploads[0].imageSubresource;
	readbacks[0].imageExtent = image_info.extent;
	readbacks[1] = readbacks[0];
	readbacks[1].bufferOffset = 624;
	readbacks[1].bufferRowLength = 17;
	readbacks[1].bufferImageHeight = 9;

	/* A bad trailing upload selection consumes its whole vector and following readback without retaining any prefix. */
	for (index = 0; index < 2; index++) {
		uploads[1].imageSubresource.mipLevel = 1;
		if (index != 0)
			uploads[1].imageSubresource.mipLevel = 0;
		begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
		vulkan_write_u64(&writer, 156);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
		writer.bytes -= 8;
		ws141_client_encode_raster_copy(&writer, 156, 196, 195, VK_IMAGE_LAYOUT_GENERAL, uploads, 2, true);
		ws141_client_encode_raster_copy(&writer, 156, 196, 195, VK_IMAGE_LAYOUT_GENERAL, readbacks, 2, false);
		vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
		vulkan_write_u32(&writer, 1);
		vulkan_write_u64(&writer, 156);
		error = execute(session, &writer, &reader);
		assert(error == 0);
		if (index == 0) {
			assert(vulkan_read_u32(&reader) != VK_SUCCESS && command->first == NULL);
			assert(image_object->references == 1 && buffer_object->references == 1);
		} else {
			assert(vulkan_read_u32(&reader) == VK_SUCCESS && image_object->references == 3 && buffer_object->references == 3);
		}
	}

	/* Runtime row-vector allocation and execution-time layout refusal leave every source and destination byte unchanged. */
	copy = (struct bcm2711_vulkan_buffer_copy *)command->first;
	assert(copy->regions.images[0].bufferRowLength == 11 && copy->regions.images[1].bufferRowLength == 0);
	memset(image_cpu, 0xd3, sizeof(expected_image));
	memset(expected_image, 0xd3, sizeof(expected_image));
	memset(buffer_cpu, 0x27, sizeof(expected_buffer));
	memset(expected_buffer, 0x27, sizeof(expected_buffer));
	baseline = allocations;
	fail_after = 1;
	error = bcm2711_vulkan_buffer_copy_run(&copy->record);
	assert(error == ENOMEM && allocations == baseline && memcmp(image_cpu, expected_image, sizeof(expected_image)) == 0);
	controller = session->render->device;
	session->render->vulkan = session;
	error = bcm2711_vulkan_native_job_create(primary, &job);
	assert(error == 0);
	image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	calls = native_execute_calls;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == EINVAL && retired && native_execute_calls == calls && memcmp(image_cpu, expected_image, sizeof(expected_image)) == 0);
	assert(memcmp(buffer_cpu, expected_buffer, sizeof(expected_buffer)) == 0);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && allocations == baseline && command->pending == 0);
	image->layout = VK_IMAGE_LAYOUT_GENERAL;

	/* Actual FIFO source reads observe host updates after preparation and preserve raw BGRA bytes without channel conversion. */
	error = bcm2711_vulkan_native_job_create(primary, &job);
	assert(error == 0);
	for (index = 0; index < sizeof(expected_buffer); index++)
		((uint8_t *)buffer_cpu)[index] = (uint8_t)(index ^ 0x5cU);
	memcpy(expected_buffer, buffer_cpu, sizeof(expected_buffer));
	for (row = 0; row < 3; row++)
		memcpy(expected_image + (row + 2U) * 64U + 12U, expected_buffer + 4U + row * 44U, 28);
	for (row = 0; row < 2; row++)
		memcpy(expected_image + row * 64U, expected_buffer + 160U + row * 8U, 8);
	for (row = 0; row < 7; row++) {
		memcpy(expected_buffer + 224U + row * 52U, expected_image + row * 64U, 52);
		memcpy(expected_buffer + 624U + row * 68U, expected_image + row * 64U, 52);
	}

	/* Complete upload followed by both readbacks changes exactly selected row bytes and performs no native GPU launch. */
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == 0 && retired && native_execute_calls == calls && job->pass == NULL);
	assert(memcmp(image_cpu, expected_image, sizeof(expected_image)) == 0 && memcmp(buffer_cpu, expected_buffer, sizeof(expected_buffer)) == 0);
	assert(image->layout == VK_IMAGE_LAYOUT_GENERAL && command->pending == 1);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && allocations == baseline && command->pending == 0);
	session->render->vulkan = NULL;

	/* A second actual buffer alias tests copied row unions against physical image placement, including legal unused image padding. */
	buffer_info.size = sizeof(expected_image);
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_BUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferCreateInfo(&writer, &buffer_info);
	error = input_created(session, &writer, 197);
	assert(error == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_BUFFER_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 197);
	vulkan_write_u64(&writer, 102);
	vulkan_write_u64(&writer, 4096);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	alias_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, 197);
	assert(alias_object != NULL);
	fixture = kern_calloc(1, sizeof(*fixture));
	assert(fixture != NULL);
	memcpy(fixture, copy, sizeof(*fixture));
	fixture->record.objects[0] = alias_object;
	fixture->record.count = 1;
	fixture->regions.images[0].bufferOffset = 52;
	fixture->regions.images[0].bufferRowLength = 16;
	fixture->regions.images[0].imageOffset.x = 0;
	fixture->regions.images[0].imageOffset.y = 0;
	fixture->regions.images[0].imageExtent.width = 2;
	fixture->regions.images[0].imageExtent.height = 2;
	error = bcm2711_vulkan_buffer_copy_validate(&fixture->record, image->device);
	assert(error == 0);
	fixture->regions.images[0].bufferOffset = 0;
	error = bcm2711_vulkan_buffer_copy_validate(&fixture->record, image->device);
	assert(error == EINVAL);
	kern_free(fixture);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_BUFFER, 197);
	assert(error == 0);

	/* Only this primary, transfer image and staging buffer retire; all earlier actual graphics owners remain valid. */
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, 156);
	assert(error == 0 && image_object->references == 1 && buffer_object->references == 1);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_BUFFER, 196);
	assert(error == 0);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_IMAGE, 195);
	assert(error == 0);
	puts("WS141 real public coherent upload/readback/raw BGRA/FIFO/padded-packed rows/whole refusal/no native launch: PASS");

	/* Succeeded: exact CPU transfer bytes are verified separately from physical native rendering. */
	return;
}

/* Checks actual coherent buffer transfers, FIFO reads and exact destination bytes without simulating any GPU operation. */
static void
buffer_copy_test(
	struct bcm2711_vulkan_session *session)
{
	struct bcm2711_vulkan_object *objects[2];
	struct bcm2711_vulkan_object *primary;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_buffer_copy *copy;
	struct bcm2711_vulkan_buffer_copy *fixture;
	struct bcm2711_vulkan_native_job *job;
	struct bcm2711_render_device *controller;
	struct bcm2711_v3d_view *view;
	struct vulkan_object pool;
	VkBufferCreateInfo info;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	VkBufferCopy regions[2];
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[4096];
	uint8_t expected[256];
	void *cpu[2];
	uint32_t address;
	uint32_t index;
	uint32_t calls;
	unsigned baseline;
	bool retired;
	int error;

	/* Two distinct actual transfer buffers bind disjoint logical slices of the existing shared coherent allocation. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = 256;
	info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_BUFFER, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkBufferCreateInfo(&writer, &info);
		error = input_created(session, &writer, 196 + index);
		assert(error == VK_SUCCESS);
		begin(&writer, wire, sizeof(wire), GPU_OP_BIND_BUFFER_MEMORY, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 196 + index);
		vulkan_write_u64(&writer, 102);
		vulkan_write_u64(&writer, 6144 + index * 256U);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		objects[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, 196 + index);
		assert(objects[index] != NULL);
		error = bcm2711_vulkan_resource_backing(objects[index]->payload, 0, 256, &view, &address, &cpu[index]);
		assert(error == 0 && view->buffer->uncached);
	}

	/* A separate real primary uses the existing reset-enabled device pool and retains no ordinary graphics state. */
	memset(&pool, 0, sizeof(pool));
	pool.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 155);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	primary = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 155);
	assert(primary != NULL);
	command = primary->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	memset(regions, 0, sizeof(regions));
	regions[0].srcOffset = 1;
	regions[0].dstOffset = 7;
	regions[0].size = 13;
	regions[1].srcOffset = 80;
	regions[1].dstOffset = 100;
	regions[1].size = 17;

	/* A late invalid byte interval consumes the whole command without retaining a prefix; a fresh exact recording succeeds. */
	for (index = 0; index < 2; index++) {
		regions[1].size = 99999;
		if (index != 0)
			regions[1].size = 17;
		begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
		vulkan_write_u64(&writer, 155);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
		writer.bytes -= 8;
		ws141_client_encode_buffer_copy(&writer, 155, 196, 197, regions, 2);
		vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
		vulkan_write_u32(&writer, 1);
		vulkan_write_u64(&writer, 155);
		error = execute(session, &writer, &reader);
		assert(error == 0);
		if (index == 0) {
			assert(vulkan_read_u32(&reader) != VK_SUCCESS && command->first == NULL);
			assert(objects[0]->references == 1 && objects[1]->references == 1);
		} else {
			assert(vulkan_read_u32(&reader) == VK_SUCCESS && objects[0]->references == 2 && objects[1]->references == 2);
		}
	}

	/* Pending preparation snapshots immutable offsets while execution reads the latest coherent source bytes in FIFO order. */
	copy = (struct bcm2711_vulkan_buffer_copy *)command->first;
	assert(copy->record.count == 2 && copy->regions.buffers[0].srcOffset == 1 && copy->regions.buffers[1].size == 17);
	controller = session->render->device;
	session->render->vulkan = session;
	baseline = allocations;
	memset(cpu[0], 0x27, 256);
	memset(cpu[1], 0xa5, 256);
	error = bcm2711_vulkan_native_job_create(primary, &job);
	assert(error == 0 && command->pending == 1);
	for (index = 0; index < 256; index++)
		((uint8_t *)cpu[0])[index] = (uint8_t)(index ^ 0x7aU);
	memset(expected, 0xa5, sizeof(expected));
	memcpy(expected + 7, (uint8_t *)cpu[0] + 1, 13);
	memcpy(expected + 100, (uint8_t *)cpu[0] + 80, 17);
	calls = native_execute_calls;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == 0 && retired && job->pass == NULL && native_execute_calls == calls);
	assert(memcmp(cpu[1], expected, sizeof(expected)) == 0 && command->pending == 1);
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == EBUSY && memcmp(cpu[1], expected, sizeof(expected)) == 0);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && allocations == baseline && command->pending == 0);
	session->render->vulkan = NULL;

	/* Independent copied validator input permits disjoint same-resource intervals and rejects physical source/destination alias. */
	fixture = kern_calloc(1, sizeof(*fixture));
	assert(fixture != NULL);
	memcpy(fixture, copy, sizeof(*fixture));
	fixture->record.objects[1] = objects[0];
	fixture->regions.buffers[0].dstOffset = 32;
	fixture->regions.buffers[1].dstOffset = 120;
	error = bcm2711_vulkan_buffer_copy_validate(&fixture->record, ((struct bcm2711_vulkan_resource *)objects[0]->payload)->device);
	assert(error == 0);
	fixture->regions.buffers[0].dstOffset = 6;
	error = bcm2711_vulkan_buffer_copy_validate(&fixture->record, ((struct bcm2711_vulkan_resource *)objects[0]->payload)->device);
	assert(error == EINVAL);
	kern_free(fixture);

	/* Only this primary and its two distinct buffers retire, leaving the shared allocation and earlier graphics graph intact. */
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, 155);
	assert(error == 0 && objects[0]->references == 1 && objects[1]->references == 1);
	for (index = 0; index < 2; index++) {
		error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_BUFFER, 196 + index);
		assert(error == 0);
	}

	/* Succeeded: CPU coherent transfer evidence does not assert any native GPU execution or completion. */
	puts("WS141 actual public coherent buffer copy/FIFO latest bytes/exact intervals/no native launch/alias/pending retirement: PASS");
	return;
}

/* Checks real public image-clear recording, full native tile lists and pending output lifetime without simulating GPU pixel writes. */
static void
clear_test(
	struct bcm2711_vulkan_session *session)
{
	struct bcm2711_vulkan_object *image_object;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_record *record;
	struct bcm2711_vulkan_native_job *job;
	struct bcm2711_vulkan_native_pass *pass;
	struct bcm2711_render_device *controller;
	struct bcm2711_v3d_view *view;
	struct vulkan_object pool;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	VkImageSubresourceRange ranges[2];
	VkImageLayout saved_layout;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint8_t wire[4096];
	uint8_t saved[16384];
	uint8_t *render_bytes;
	void *cpu;
	uint64_t available;
	uint32_t address;
	uint32_t references;
	uint32_t calls;
	uint32_t index;
	unsigned baseline;
	bool retired;
	int error;

	/* Existing actual typed texture storage already has transfer-destination usage and an independently bound coherent backing. */
	image_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, 180);
	assert(image_object != NULL);
	image = image_object->payload;
	saved_layout = image->layout;
	image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	assert(error == 0 && image->bytes <= sizeof(saved));
	memcpy(saved, cpu, image->bytes);
	references = image_object->references;
	controller = session->render->device;

	/* A distinct real primary uses the existing reset-enabled pool without changing the retained graphics recording. */
	memset(&pool, 0, sizeof(pool));
	pool.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 153);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 153);
	assert(object != NULL);
	command = object->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	memset(ranges, 0, sizeof(ranges));
	for (index = 0; index < 2; index++) {
		ranges[index].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ranges[index].levelCount = 1;
		ranges[index].layerCount = 1;
	}

	/* A bad trailing range is fully consumed, reports End failure, and retains no partial output edge. */
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
		vulkan_write_u64(&writer, 153);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		ranges[1].baseMipLevel = 1;
		if (index != 0)
			ranges[1].baseMipLevel = 0;
		ranges[1].levelCount = VK_REMAINING_MIP_LEVELS;
		ranges[1].layerCount = VK_REMAINING_ARRAY_LAYERS;
		begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
		writer.bytes -= 8;
		ws141_client_encode_clear(&writer, 153, 180, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ranges, 2);
		vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
		vulkan_write_u32(&writer, 1);
		vulkan_write_u64(&writer, 153);
		error = execute(session, &writer, &reader);
		assert(error == 0);
		if (index == 0) {
			assert(vulkan_read_u32(&reader) != VK_SUCCESS && command->first == NULL && command->recording_error == EINVAL);
			assert(image_object->references == references);
		} else {
			assert(vulkan_read_u32(&reader) == VK_SUCCESS && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
		}
	}

	/* The complete actual client node owns raw colour, declared layout and one independently retained typed image. */
	record = (struct bcm2711_vulkan_record *)command->first;
	assert(record != NULL && command->first == command->last && record->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE);
	assert(record->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && record->count == 2 && record->words[2] == 0x3f800000U);
	assert(image_object->references == references + 1);
	session->render->vulkan = session;
	baseline = allocations;
	error = bcm2711_vulkan_native_job_create(object, &job);
	assert(error == 0 && command->pending == 1 && job->prepared->first->record == record);

	/* A late native budget refusal retires an allocated list prefix and independent output hold without changing caller budget or pixels. */
	available = 4096;
	error = bcm2711_vulkan_native_clear_create(&controller->space, job->prepared->first, &available, &pass);
	assert(error == ENOMEM && pass == NULL && available == 4096 && memcmp(cpu, saved, image->bytes) == 0);

	/* A complete meta pass uses zero draws, no load and full tile coverage; the real RCL contains the expected distinct packed clear channels. */
	available = 256ULL * 1024U * 1024U;
	error = bcm2711_vulkan_native_clear_create(&controller->space, job->prepared->first, &available, &pass);
	assert(error == 0 && pass != NULL && pass->draws == 0 && pass->first == NULL && pass->count == 9);
	assert(pass->state.load == 0 && pass->state.store == 1 && pass->load == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	assert(pass->state.last_x == (image->width - 1U) / 64U && pass->state.last_y == (image->height - 1U) / 64U);
	assert(pass->initial_layout == record->layout && pass->final_layout == record->layout && pass->target == image);
	render_bytes = pass->storage[1]->view->buffer->address;
	assert(render_bytes[11] == 64 && render_bytes[12] == 128 && render_bytes[13] == 255 && render_bytes[14] == 255);
	assert(pass->job.command.cl.bin_end - pass->job.command.cl.bin_start == 14);
	error = bcm2711_vulkan_native_pass_release(&pass, true);
	assert(error == 0 && pass == NULL && memcmp(cpu, saved, image->bytes) == 0);

	/* FIFO layout mismatch refuses before launch or CPU framebuffer mutation and the whole pending graph remains disposable. */
	calls = native_execute_calls;
	image->layout = VK_IMAGE_LAYOUT_GENERAL;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == EINVAL && retired && native_execute_calls == calls && memcmp(cpu, saved, image->bytes) == 0);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && command->pending == 0 && allocations == baseline);
	image->layout = record->layout;

	/* Successful host handoff runs one real native clear job, while the explicit mock runner writes no pixel and supplies no physical rendering proof. */
	error = bcm2711_vulkan_native_job_create(object, &job);
	assert(error == 0);
	native_execute_error = 0;
	native_execute_retired = true;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == 0 && retired && job->pass == NULL && command->pending == 1 && native_execute_calls == calls + 1);
	assert(image->layout == record->layout && memcmp(cpu, saved, image->bytes) == 0);
	error = bcm2711_vulkan_native_job_dispose(controller, job, true);
	assert(error == 0 && command->pending == 0 && allocations == baseline);

	/* A synthetic unretired clear keeps its zero-draw native owner and the whole pending primary until explicit host reset admission. */
	error = bcm2711_vulkan_native_job_create(object, &job);
	assert(error == 0);
	native_execute_error = ETIMEDOUT;
	native_execute_retired = false;
	error = bcm2711_vulkan_native_job_execute(controller, session->render, job, &retired);
	assert(error == ETIMEDOUT && !retired && job->pass != NULL && job->pass->first == NULL && job->pass->count == 9);
	assert(job->pass->output == view && command->pending == 1 && memcmp(cpu, saved, image->bytes) == 0);
	error = bcm2711_vulkan_native_job_dispose(controller, job, false);
	assert(error == 0 && controller->quarantine == job && command->pending == 1);
	error = bcm2711_vulkan_native_jobs_recover(controller);
	assert(error == 0 && controller->quarantine == NULL && command->pending == 0 && allocations == baseline);
	native_execute_error = 0;
	native_execute_retired = true;
	session->render->vulkan = NULL;
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, 153);
	assert(error == 0 && image_object->references == references);
	image->layout = saved_layout;
	puts("WS141 actual public image clear/full native tile clear/no CPU pixel writes/FIFO layout/owned rollback: PASS");

	/* Succeeded: clear uses the existing pending/native retirement path and leaves all unrelated graphics recordings intact. */
	return;
}

/* Observes borrowed valid native state without claiming independently owned GPU preparation or execution. */
static int
draw_observe(
	void *payload,
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record)
{
	uint32_t *observation;

	/* Pass begin, valid draw and pass end are the only native preparation points. */
	observation = payload;
	assert(state->pass != NULL);
	observation[0]++;
	if (record->opcode == GPU_OP_CMD_DRAW) {
		assert(state->pipeline != NULL && state->viewport != NULL && state->scissor != NULL);
		assert(state->vertices[0]->bytes == 48 && state->offsets[0] == 0);
		assert(state->sets[0].set->bindings[0].view->identity == 181);
		assert(state->push[0][0] == 0x3f000000U && state->push_layouts[0][0] != NULL && state->push_layouts[1][0] == NULL);
		observation[2]++;
		if (observation[3] == 1)
			assert(state->sets[1].set != NULL);
		if (observation[3] == 2)
			assert(state->sets[1].set == NULL && state->sets[1].layout != NULL);
	}

	/* A selected callback refusal tests caller rollback boundaries without starting native DMA. */
	if (observation[1] == record->opcode)
		return ENOMEM;

	/* Succeeded: exact borrowed pass/draw state was observed under the serialized preparation contract. */
	return 0;
}

/* Runs actual public client vkCmd calls through native recording, first-error handling and retained graph retirement. */
static void
record_test(
	struct bcm2711_vulkan_session *session)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_object client;
	struct bcm2711_vulkan_object *command_object;
	struct bcm2711_vulkan_object *set_object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_record *event;
	struct bcm2711_vulkan_command_node *node;
	VkBufferCreateInfo buffer_info;
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo image_view_info;
	VkSamplerCreateInfo sampler_info;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	VkCommandPoolCreateInfo command_info;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	uint8_t wire[8192];
	uint32_t index;
	uint32_t nodes;
	unsigned baseline;
	int error;

	/* Real bound vertex storage uses a nonoverlapping interval in the existing coherent native fixture allocation. */
	baseline = allocations;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = 48;
	buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_BUFFER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferCreateInfo(&writer, &buffer_info);
	error = input_created(session, &writer, 170);
	assert(error == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_BUFFER_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 170);
	vulkan_write_u64(&writer, 102);
	vulkan_write_u64(&writer, 1024);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);

	/* Draw validation uses an actual distinct bound sampled image, avoiding undefined attachment feedback in the earlier recording-only fixture. */
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
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, &image_info);
	error = input_created(session, &writer, 180);
	assert(error == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_BIND_IMAGE_MEMORY, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 180);
	vulkan_write_u64(&writer, 102);
	vulkan_write_u64(&writer, 2048);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	memset(&client, 0, sizeof(client));
	client.wire_id = 180;
	memset(&image_view_info, 0, sizeof(image_view_info));
	image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	image_view_info.image = (VkImage)(uintptr_t)&client;
	image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	image_view_info.format = image_info.format;
	image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	image_view_info.subresourceRange.levelCount = 1;
	image_view_info.subresourceRange.layerCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_IMAGE_VIEW, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageViewCreateInfo(&writer, &image_view_info);
	error = input_created(session, &writer, 181);
	assert(error == VK_SUCCESS);

	/* The quad's exact retained set interface is allocated with normal native pool ownership and mutable sampled inputs. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SAMPLER, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSamplerCreateInfo(&writer, &sampler_info);
	error = input_created(session, &writer, 161);
	assert(error == VK_SUCCESS);
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 2;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_DESCRIPTOR_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkDescriptorPoolCreateInfo(&writer, &pool_info);
	error = input_created(session, &writer, 160);
	assert(error == VK_SUCCESS);
	error = sets_allocate(session, 160, 130, 163);
	assert(error == 0);
	set_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, 163);
	assert(set_object != NULL);
	set = set_object->payload;
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 163, 161, 181);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0);

	/* Reset-enabled ordinary primary metadata matches the real client wrapper's local recording identity. */
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandPoolCreateInfo(&writer, &command_info);
	error = input_created(session, &writer, 150);
	assert(error == VK_SUCCESS);
	memset(&client, 0, sizeof(client));
	client.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&client;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 151);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	command_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 151);
	assert(command_object != NULL);
	command = command_object->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	/* A fifth native event allocation refusal must consume the rest of the real stream, report End OOM, and permit clean re-recording. */
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
		vulkan_write_u64(&writer, 151);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
		writer.bytes -= 8;
		ws141_client_encode_recording(&writer, 151);
		vulkan_write_u32(&writer, GPU_OP_END_COMMAND_BUFFER);
		vulkan_write_u32(&writer, 1);
		vulkan_write_u64(&writer, 151);
		if (index == 0)
			fail_after = 5;
		error = execute(session, &writer, &reader);
		assert(error == 0);
		if (index == 0) {
			assert(vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
			assert(command->recording_error == ENOMEM && command->state == BCM2711_VULKAN_COMMAND_INVALID);
		} else {
			assert(vulkan_read_u32(&reader) == VK_SUCCESS && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE && !command->render_open);
		}
	}

	/* Exact nine-event ordering, selected clear union and raw push words come from real public client encoding. */
	nodes = 0;
	for (node = command->first; node != NULL; node = node->next)
		nodes++;
	assert(nodes == 9);
	event = (struct bcm2711_vulkan_record *)command->first;
	assert(event->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS && event->count == 2 && event->words[0] == 0x3e800000U && event->words[3] == 0x3f800000U);
	event = (struct bcm2711_vulkan_record *)event->node.next;
	assert(event->opcode == GPU_OP_CMD_SET_VIEWPORT && event->words[2] == 0x41800000U && event->words[3] == 0x41000000U);
	assert(set_object->references == 2);

	/* Native state walking consumes the complete actual client recording before any GPU work is prepared. */
	draw_test(command);
	prepared_test(session, command_object);

	/* Explicit dependencies use the actual public client encoder and a separately owned native primary. */
	barrier_test(session, command_object);
	clear_test(session);
	transfer_test(session);
	buffer_copy_test(session);
	raster_copy_test(session);
	queue_test(session, command_object);

	/* Explicit pending fixture ownership refuses mutation before changing a set generation or any resource edge. */
	set->pending = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 163, 161, 104);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == EBUSY && set->bindings[0].view->identity == 181);
	error = bcm2711_vulkan_record_current(command);
	assert(error == 0 && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
	set->pending = 0;

	/* Vulkan 1.0 ordinary update-after-bind invalidates the executable recording while preserving independent owned resources. */
	begin(&writer, wire, sizeof(wire), GPU_OP_UPDATE_DESCRIPTOR_SETS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	encode_image_write(&writer, 163, 161, 104);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && set->bindings[0].view->identity == 104);
	error = bcm2711_vulkan_record_current(command);
	assert(error == EINVAL && command->state == BCM2711_VULKAN_COMMAND_INVALID && command->first != NULL);

	/* Withdrawing every public input leaves the retained old primary graph intact, then final release returns all added heap ownership. */
	error = bcm2711_vulkan_object_retain(command_object);
	assert(error == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_COMMAND_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 150);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && !command_object->published && command->first != NULL);
	destroy(session, GPU_OP_DESTROY_BUFFER, 170);
	destroy(session, GPU_OP_DESTROY_SAMPLER, 161);
	destroy(session, GPU_OP_DESTROY_IMAGE_VIEW, 181);
	destroy(session, GPU_OP_DESTROY_IMAGE, 180);
	begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_DESCRIPTOR_POOL, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 160);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && !set_object->published && set_object->references == 1 && set->bindings[0].view->identity == 104);
	error = bcm2711_vulkan_object_release(command_object);
	assert(error == 0 && allocations == baseline);
	puts("WS141 Vulkan actual public vkCmd stream/order/clear union/record OOM/update invalidation/retained primary graph: PASS");

	/* Succeeded: recording software is verified independently of native CL execution and actual GPU completion. */
	return;
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

	/* Complete queue submits own native retirement independently of the decoder trailer. */
	error = bcm2711_vulkan_queue_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Core native fences and semaphores keep their independent typed device owners. */
	error = bcm2711_vulkan_sync_dispatch(session, opcode, requested, reader, reply, &handled);
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
	/* Fully compiled graphics batches publish independent typed owners only after complete selected-state decoding. */
	if (handled != 0)
		return 0;
	error = bcm2711_vulkan_pipeline_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Primary command pools preserve native ownership through reset, recording and public identity retirement. */
	error = bcm2711_vulkan_command_pool_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;
	error = bcm2711_vulkan_command_batch_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;
	error = bcm2711_vulkan_command_buffer_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Complete explicit dependencies retain typed resources independently of the public handles. */
	error = bcm2711_vulkan_barrier_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Coherent buffer copies retain complete actual typed byte intervals independently of graphics state. */
	error = bcm2711_vulkan_buffer_copy_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Reply-free copied image transfers retain both exact typed resources and the complete region vector. */
	error = bcm2711_vulkan_transfer_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled != 0)
		return 0;

	/* Reply-free ordered graphics records retain immutable state and exact input identities. */
	error = bcm2711_vulkan_record_dispatch(session, opcode, requested, reader, reply, &handled);
	if (error != 0)
		return error;
	if (handled == 0)
		return ENOTSUP;

	/* Succeeded: one real typed native module consumed the complete selected command. */
	return 0;
}

/* Exercises the unchanged actual client submit encoder across full native preparation, binary chains, fence retirement and uncertainty. */
static void
queue_test(
    struct bcm2711_vulkan_session *session,
    struct bcm2711_vulkan_object *command_object)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_vulkan_session *old_namespace;
	struct bcm2711_vulkan_object *fence_object;
	struct bcm2711_vulkan_object *sem_object;
	struct bcm2711_vulkan_object *once_object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_command_buffer *once;
	struct bcm2711_vulkan_sync *fence;
	struct bcm2711_vulkan_sync *semaphore;
	struct vulkan_object pool;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkFenceCreateInfo create;
	VkSemaphoreCreateInfo sem_create;
	VkCommandBufferAllocateInfo allocation;
	VkCommandBufferBeginInfo recording;
	uint8_t wire[2048];
	uint32_t index;
	uint32_t calls;
	unsigned baseline;
	unsigned prepared_baseline;
	int error;

	/* The actual controller/primary graph already exists; the explicit host runner never performs physical DMA or QPU execution. */
	controller = session->render->device;
	old_namespace = session->render->vulkan;
	session->render->vulkan = session;
	command = command_object->payload;
	baseline = allocations;
	calls = native_execute_calls;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_FENCE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkFenceCreateInfo(&writer, &create);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 900);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	fence_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FENCE, 900);
	assert(fence_object != NULL);
	fence = fence_object->payload;
	memset(&sem_create, 0, sizeof(sem_create));
	sem_create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SEMAPHORE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSemaphoreCreateInfo(&writer, &sem_create);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 901);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	sem_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SEMAPHORE, 901);
	assert(sem_object != NULL);
	semaphore = sem_object->payload;
	prepared_baseline = allocations;

	/* An empty actual submit signals its ordinary native fence after preceding FIFO work without launching a GPU job. */
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 900, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && fence->signaled && !fence->pending);
	assert(native_execute_calls == calls && allocations == prepared_baseline);
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_FENCES, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 900);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && !fence->signaled);

	/* A real signal-only batch completes one binary signal; repeating it without a consumption changes no live payload. */
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 0, 1);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && semaphore->signaled);
	assert(semaphore->signals_reserved == 1 && semaphore->signals_completed == 1 && semaphore->waits_completed == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 900, 1);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && semaphore->signaled && !fence->signaled && !fence->pending);
	assert(allocations == prepared_baseline && native_execute_calls == calls);

	/* A late absent second primary unwinds an already prepared first primary and both proposed binary transitions without any live effect. */
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 900, 2);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && command->pending == 0 && !fence->signaled && !fence->pending);
	assert(semaphore->signals_completed == 1 && semaphore->waits_completed == 0 && semaphore->signaled);
	assert(allocations == prepared_baseline && native_execute_calls == calls);

	/* HOST is forbidden for submit waits and COMPUTE is absent from this graphics queue; neither refusal consumes a prior native signal. */
	for (index = 5; index <= 6; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
		ws141_client_encode_submit(&writer, 151, 901, 900, index);
		error = execute(session, &writer, &reader);
		assert(error == ENOTSUP && semaphore->signaled && !fence->pending && !fence->signaled);
		assert(command->pending == 0 && allocations == prepared_baseline && native_execute_calls == calls);
	}

	/* Early and partial CPU preparation OOM return ordinary Vulkan allocation failure with every input state unchanged. */
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
		ws141_client_encode_submit(&writer, 151, 901, 900, 3);
		if (index == 0) {
			fail_after = 1;
		} else {
			fail_after = 5;
		}

		/* Both refusal points occur before live semaphore/fence publication or the explicit mock native runner. */
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		assert(command->pending == 0 && !fence->signaled && !fence->pending && semaphore->signaled);
		assert(semaphore->signals_completed == 1 && semaphore->waits_completed == 0);
		assert(allocations == prepared_baseline && native_execute_calls == calls);
	}

	/* Actual wait-primary-signal-wait ordering completes the real native pass before its following signals and final fence. */
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 900, 3);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && native_execute_calls == calls + 1U);
	assert(command->pending == 0 && command->state == BCM2711_VULKAN_COMMAND_EXECUTABLE);
	assert(semaphore->signals_reserved == 2 && semaphore->signals_completed == 2);
	assert(semaphore->waits_reserved == 2 && semaphore->waits_completed == 2 && !semaphore->signaled && !semaphore->pending);
	assert(fence->signaled && !fence->pending && allocations == prepared_baseline);

	/* A real independently recorded empty one-time primary becomes invalid only after actual accepted execution, with no GPU launch. */
	memset(&pool, 0, sizeof(pool));
	pool.wire_id = 150;
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = (VkCommandPool)(uintptr_t)&pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	begin(&writer, wire, sizeof(wire), GPU_OP_ALLOCATE_COMMAND_BUFFERS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, &allocation);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 157);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	once_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, 157);
	assert(once_object != NULL);
	once = once_object->payload;
	memset(&recording, 0, sizeof(recording));
	recording.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	recording.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin(&writer, wire, sizeof(wire), GPU_OP_BEGIN_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 157);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &recording);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_END_COMMAND_BUFFER, 1);
	vulkan_write_u64(&writer, 157);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 157, 901, 0, 4);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS && once->state == BCM2711_VULKAN_COMMAND_INVALID && once->pending == 0);
	assert(native_execute_calls == calls + 1U);
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 157, 901, 0, 4);
	error = execute(session, &writer, &reader);
	assert(error == EINVAL && once->state == BCM2711_VULKAN_COMMAND_INVALID && once->pending == 0);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, 157);
	assert(error == 0 && allocations == prepared_baseline);

	/* An uncertain real native-job handoff reports DEVICE_LOST and keeps the whole pending primary instead of signaling its fence. */
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_FENCES, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 900);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	native_execute_error = ETIMEDOUT;
	native_execute_retired = false;
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 900, 4);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(controller->worker.uncertain && controller->space.native->hardware.faulted && controller->quarantine != NULL);
	assert(command->pending == 1 && !fence->signaled && !fence->pending && fence->status == VK_ERROR_DEVICE_LOST);

	/* Empty work cannot hide native uncertainty behind a fresh decoder reply or newly signaled fence. */
	begin(&writer, wire, sizeof(wire), GPU_OP_QUEUE_SUBMIT, 1);
	ws141_client_encode_submit(&writer, 151, 901, 0, 0);
	error = execute(session, &writer, &reader);
	assert(error == EIO && !fence->signaled && command->pending == 1);

	/* No physical DMA ran; explicit synthetic reset admission permits actual whole-owner retirement without claiming a production reset. */
	native_execute_error = 0;
	native_execute_retired = true;
	controller->space.native->hardware.faulted = false;
	controller->worker.uncertain = false;
	error = bcm2711_vulkan_native_jobs_recover(controller);
	assert(error == 0 && controller->quarantine == NULL && command->pending == 0 && allocations == prepared_baseline);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_FENCE, 900);
	assert(error == 0);
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_SEMAPHORE, 901);
	assert(error == 0 && allocations == baseline);
	session->render->vulkan = old_namespace;
	puts("WS141 actual client QueueSubmit/full CPU preflight/binary chains/native fence/one-time/OOM/whole uncertain quarantine: PASS");

	/* Succeeded: actual queue and native primary source respects complete input, retirement and independent synchronization boundaries. */
	return;
}

/* Exercises exact client sync records, native payload observation and atomic reset/destruction across independent pending owners. */
static void
sync_test(
    struct bcm2711_vulkan_session *session)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *objects[3];
	struct bcm2711_vulkan_object *found;
	struct bcm2711_vulkan_sync *fence;
	struct bcm2711_vulkan_sync *other;
	struct bcm2711_vulkan_sync *semaphore;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkFenceCreateInfo create;
	VkSemaphoreCreateInfo sem_create;
	uint8_t wire[512];
	uint32_t index;
	uint32_t references;
	unsigned baseline;
	int error;

	/* The real client codec emits the flags-only native shape read from sync_create, without fabricated public API or GPU completion. */
	baseline = allocations;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, 30);
	assert(device != NULL);
	references = device->references;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	create.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_FENCE, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkFenceCreateInfo(&writer, &create);
		vulkan_write_u64(&writer, 0);
		vulkan_write_u64(&writer, 1);
		vulkan_write_u64(&writer, 90U + index);
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
		assert(vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == 90U + index);
		objects[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FENCE, 90U + index);
		assert(objects[index] != NULL);
	}

	/* An ordinary binary semaphore starts with no reserved or completed signal/wait and owns its independent device edge. */
	memset(&sem_create, 0, sizeof(sem_create));
	sem_create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_SEMAPHORE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSemaphoreCreateInfo(&writer, &sem_create);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 92);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == 92);
	objects[2] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SEMAPHORE, 92);
	assert(objects[2] != NULL && device->references == references + 3U);
	fence = objects[0]->payload;
	other = objects[1]->payload;
	semaphore = objects[2]->payload;
	assert(fence->signaled && other->signaled && !semaphore->signaled);
	assert(semaphore->signals_reserved == 0 && semaphore->waits_reserved == 0);
	assert(semaphore->signals_completed == 0 && semaphore->waits_completed == 0);

	/* Payload and registry allocation failures return ordinary Vulkan OOM with zero output and no retained parent residue. */
	for (index = 1; index <= 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_FENCE, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 1);
		vulkan_encode_VkFenceCreateInfo(&writer, &create);
		vulkan_write_u64(&writer, 0);
		vulkan_write_u64(&writer, 1);
		vulkan_write_u64(&writer, 93);
		fail_after = index;
		error = execute(session, &writer, &reader);
		assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		assert(vulkan_read_u64(&reader) == 1 && vulkan_read_u64(&reader) == 0);
		found = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FENCE, 93);
		assert(found == NULL && device->references == references + 3U && allocations == baseline + 6U);
	}

	/* A trailing invalid ID or explicitly modeled pending owner refuses reset without unsignaling an earlier selected payload. */
	for (index = 0; index < 2; index++) {
		begin(&writer, wire, sizeof(wire), GPU_OP_RESET_FENCES, 1);
		vulkan_write_u64(&writer, 30);
		vulkan_write_u32(&writer, 2);
		vulkan_write_u64(&writer, 2);
		vulkan_write_u64(&writer, 90);
		if (index == 0) {
			vulkan_write_u64(&writer, 93);
		} else {
			other->pending = true;
			vulkan_write_u64(&writer, 91);
		}

		/* Only explicit pending metadata is synthetic; the decoder and both selected fence owners are actual source. */
		error = execute(session, &writer, &reader);
		if (index == 0) {
			assert(error == EINVAL);
		} else {
			assert(error == EBUSY);
		}

		/* Neither refused complete vector published an earlier reset. */
		assert(fence->signaled && other->signaled);
	}

	/* A pending native payload is never reported signaled even when its retained prior flag was true. */
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_FENCE_STATUS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 91);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_NOT_READY);
	other->pending = false;
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_FENCE_STATUS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 91);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);

	/* IRQ/controller loss takes precedence over an initially signaled payload. */
	session->render->device->space.native->hardware.faulted = true;
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_FENCE_STATUS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 91);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_DEVICE_LOST);
	session->render->device->space.native->hardware.faulted = false;

	/* One complete reset publishes both unsignaled payloads after all IDs passed preflight. */
	begin(&writer, wire, sizeof(wire), GPU_OP_RESET_FENCES, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u32(&writer, 2);
	vulkan_write_u64(&writer, 2);
	vulkan_write_u64(&writer, 90);
	vulkan_write_u64(&writer, 91);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_SUCCESS);
	assert(!fence->signaled && !other->signaled);
	begin(&writer, wire, sizeof(wire), GPU_OP_GET_FENCE_STATUS, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 90);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == VK_NOT_READY);

	/* Actual namespace removal preserves an explicit independent pending reference until its exact owner releases it. */
	error = bcm2711_vulkan_object_retain(objects[0]);
	assert(error == 0);
	begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_FENCE, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 90);
	vulkan_write_u64(&writer, 0);
	error = execute(session, &writer, &reader);
	assert(error == 0 && !objects[0]->published && objects[0]->references == 1);
	assert(fence->device == device && device->references == references + 3U);
	error = bcm2711_vulkan_object_release(objects[0]);
	assert(error == 0 && device->references == references + 2U);
	for (index = 1; index < 3; index++) {
		if (index == 2) {
			begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_SEMAPHORE, 1);
		} else {
			begin(&writer, wire, sizeof(wire), GPU_OP_DESTROY_FENCE, 1);
		}

		/* The selected destruction consumes the exact remaining device-owned identity. */
		vulkan_write_u64(&writer, 30);
		vulkan_write_u64(&writer, 90U + index);
		vulkan_write_u64(&writer, 0);
		error = execute(session, &writer, &reader);
		assert(error == 0);
	}

	/* Every independently retained native parent and CPU payload has retired without a physical GPU or queue completion claim. */
	assert(allocations == baseline && device->references == references);
	puts("WS141 exact client sync records/native initial status/atomic reset/pending owner/OOM/fault precedence: PASS");

	/* Succeeded: core native fence and binary semaphore ownership matches the actual finite client wire boundary. */
	return;
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

	/* Exact actual export declarations qualify the explicit native/external barrier boundary before any provider execution claim. */
	external_barrier_test(object);

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

	/* An actual private declaration cannot acquire external queue ownership merely by selecting a special family number. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, 52);
	assert(object != NULL);
	external_barrier_test(object);
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

/* Checks explicit external-family admission against actual native memory declarations using a numerical image-description fixture. */
static void
external_barrier_test(
	struct bcm2711_vulkan_object *memory_object)
{
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_vulkan_barrier *barrier;
	struct bcm2711_vulkan_resource resource;
	struct bcm2711_vulkan_object image;
	unsigned baseline;
	int expected;
	int error;

	/* This synthetic description borrows an actual typed allocation declaration and never publishes a resource or native job. */
	baseline = allocations;
	memory = memory_object->payload;
	memset(&resource, 0, sizeof(resource));
	resource.device = memory->device;
	resource.memory = memory_object;
	memset(&image, 0, sizeof(image));
	image.kind = I915_VK_OBJ_IMAGE;
	image.payload = &resource;
	barrier = kern_calloc(1, sizeof(*barrier));
	assert(barrier != NULL);

	/* One full colour transition uses the actual sole native queue and a declared external-sharing endpoint. */
	barrier->count = 1;
	barrier->source = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	barrier->destination = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	barrier->entries[0].object = &image;
	barrier->entries[0].before = VK_IMAGE_LAYOUT_GENERAL;
	barrier->entries[0].after = VK_IMAGE_LAYOUT_GENERAL;
	barrier->entries[0].range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier->entries[0].range.levelCount = 1;
	barrier->entries[0].range.layerCount = 1;
	barrier->entries[0].source_family = 0;
	barrier->entries[0].destination_family = BCM2711_VULKAN_EXTERNAL_FAMILY;
	expected = EINVAL;
	if (memory->external_type != 0)
		expected = 0;
	error = bcm2711_vulkan_barrier_validate(barrier, memory->device);
	assert(error == expected);
	barrier->entries[0].source_family = BCM2711_VULKAN_EXTERNAL_FAMILY;
	barrier->entries[0].destination_family = 0;
	error = bcm2711_vulkan_barrier_validate(barrier, memory->device);
	assert(error == expected);

	/* Unrelated native families and mixed ignored/external indices have no implemented ownership or provider contract. */
	barrier->entries[0].source_family = 7;
	error = bcm2711_vulkan_barrier_validate(barrier, memory->device);
	assert(error == ENOTSUP);
	barrier->entries[0].source_family = VK_QUEUE_FAMILY_IGNORED;
	barrier->entries[0].destination_family = BCM2711_VULKAN_EXTERNAL_FAMILY;
	error = bcm2711_vulkan_barrier_validate(barrier, memory->device);
	assert(error == ENOTSUP);
	kern_free(barrier);
	assert(allocations == baseline);

	/* Succeeded: declared shareability gates both external directions without changing any actual allocation or native owner. */
	return;
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

	/* Native command recording uses this complete target together with actual quad pipeline resources. */
	pipeline_test(session);
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
	struct vulkan_reader reader;
	struct bcm2711_vulkan_object *published;
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

	/* Native packed fetch now supplies the quad shader's missing second float component with Vulkan's zero default. */
	attribute.format = VK_FORMAT_R32_SFLOAT;
	error = bcm2711_vulkan_pipeline_build(session, device, &info, &pipeline);
	assert(error == 0 && pipeline != NULL && pipeline->attributes[0].format == VK_FORMAT_R32_SFLOAT);
	error = bcm2711_vulkan_pipeline_release(session, pipeline);
	assert(error == 0 && allocations == with_inputs && layout->references == 1);
	attribute.format = VK_FORMAT_R32G32_SFLOAT;
	error = bcm2711_vulkan_pipeline_build(session, device, &info, &pipeline);
	assert(error == 0 && pipeline != NULL && layout->references == 2);
	assert(pipeline->blend && pipeline->binding_count == 1 && pipeline->attributes[0].format == VK_FORMAT_R32G32_SFLOAT);
	assert(pipeline->programs[0]->vpm_output_words == 6 && pipeline->programs[1]->input_count == 2 && pipeline->programs[2]->varying_count == 2);
	assert(pipeline->programs[2]->code_count != 0 && pipeline->programs[2]->uniform_count != 0);

	/* The real client selected-state encoder and native decoder preserve legitimate partial batch results. */
	begin(&writer, wire, sizeof(wire), GPU_OP_CREATE_GRAPHICS_PIPELINES, 1);
	vulkan_write_u64(&writer, 30);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u32(&writer, 2);
	vulkan_write_u64(&writer, 2);
	ws141_client_encode_graphics(&writer, &info);
	info.layout = (VkPipelineLayout)(uintptr_t)132;
	ws141_client_encode_graphics(&writer, &info);
	info.layout = (VkPipelineLayout)(uintptr_t)131;
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 2);
	vulkan_write_u64(&writer, 135);
	vulkan_write_u64(&writer, 136);
	error = execute(session, &writer, &reader);
	assert(error == 0 && vulkan_read_u32(&reader) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(vulkan_read_u64(&reader) == 2 && vulkan_read_u64(&reader) == 135 && vulkan_read_u64(&reader) == 0);
	published = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE, 135);
	assert(published != NULL);

	/* Real public client recording keeps this compiled pipeline and the complete bound framebuffer live. */
	record_test(session);
	error = bcm2711_vulkan_object_retain(published);
	assert(error == 0);
	destroy(session, GPU_OP_DESTROY_PIPELINE, 135);
	assert(published->references == 1 && !published->published);
	error = bcm2711_vulkan_object_release(published);
	assert(error == 0);

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
