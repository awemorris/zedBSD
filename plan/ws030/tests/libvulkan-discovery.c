/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Verifies composite GPU discovery and logical-device ownership with a stateful native peer.
 * The peer independently parses command envelopes and device/queue inputs. Fixed property
 * payloads use the shared typed codec; this fixture checks their integration and filtering,
 * while codec field-layout tests remain a separate responsibility.
 */

#include "internal.h"
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A decoder cursor remains bounded by one immutable production command. */
struct discovery_cursor {
	const uint8_t *data;
	size_t bytes;
	size_t offset;
};

/* A native logical device owns its assigned queue timelines until destruction. */
struct discovery_device {
	uint64_t identity;
	uint64_t timelines;
	uint32_t family_counts[3];
};

/* One modeled GPU namespace is independent from every other open GPU session. */
struct discovery_peer {
	struct vulkan_context *context;
	uint64_t instance;
	uint64_t physical;
	unsigned node;
	struct discovery_device devices[80];
};

/* Namespace models remain live only between context_open and context_close. */
static struct discovery_peer peers[4];

/* Directory traversal is serial and exposes noncontiguous ordinary GPU names. */
static unsigned directory_position;

/* The one directory entry buffer follows readdir's reuse contract. */
static struct dirent directory_entry;

/* These counters observe ownership and side effects across complete fixture scenarios. */
static unsigned opened, closed, submissions, queue_requests, native_devices, native_destroys;

/* Failure controls select one bounded allocation or response in a setup path. */
static unsigned allocation_attempt, fail_allocation, fail_queue, bad_physical_echo, fail_device;

/* Queue capability controls alter only the second GPU during bounded incompatibility scenarios. */
static unsigned native_family_count, native_zero_queue;

/* Callback accounting verifies instance inheritance and compatible destruction policies. */
static unsigned allocated, freed, destruction_frees;

/* Current and compatible replacement user-data tokens belong to the entire fixture. */
static int allocation_user, destruction_user;

static uint32_t discovery_u32(struct discovery_cursor *cursor);
static uint64_t discovery_u64(struct discovery_cursor *cursor);
static void discovery_string(struct discovery_cursor *cursor);
static void discovery_structure(struct discovery_cursor *cursor, uint32_t type);
static struct discovery_peer *discovery_peer_get(struct vulkan_context *context);
static struct discovery_device *discovery_device_get(struct discovery_peer *peer, uint64_t identity);
static void discovery_instance(struct discovery_peer *peer, struct discovery_cursor *cursor, struct vulkan_writer *response);
static void discovery_enumerate(struct discovery_peer *peer, struct discovery_cursor *cursor, struct vulkan_writer *response);
static void discovery_properties(struct discovery_peer *peer, uint32_t opcode, struct discovery_cursor *cursor, struct vulkan_writer *response);
static void discovery_queues(struct discovery_peer *peer, struct discovery_cursor *cursor, struct vulkan_writer *response);
static void discovery_create_device(struct discovery_peer *peer, struct discovery_cursor *cursor, struct vulkan_writer *response);
static void discovery_get_queue(struct discovery_peer *peer, struct discovery_cursor *cursor, struct vulkan_writer *response);
static void discovery_copy_reply(const struct vulkan_writer *command, struct vulkan_writer *response, struct vulkan_reader *reader, size_t capacity);
static void *discovery_allocate(void *user, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void *discovery_reallocate(void *user, void *original, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void discovery_free(void *user, void *pointer);
static VkInstance discovery_create_instance(const VkAllocationCallbacks *allocator);
static void discovery_check_physical(VkInstance instance, VkPhysicalDevice *physical);
static void discovery_check_devices(VkPhysicalDevice *physical, const VkAllocationCallbacks *allocator, const VkAllocationCallbacks *destroy_allocator);
static void discovery_check_failures(const VkAllocationCallbacks *allocator);
static void discovery_check_extensions(void);
static void discovery_check_queue_incompatibility(const VkAllocationCallbacks *allocator);

/*
 * Starts one bounded ordinary /dev enumeration without touching host device nodes.
 */
DIR *
discovery_test_opendir(
	const char *path)
{
	/* The implementation must discover ordinary GPU nodes from the device directory. */
	assert(strcmp(path, "/dev") == 0);
	directory_position = 0U;

	/* Succeeded: this opaque token owns the current serial traversal. */
	return (DIR *)&directory_position;
}

/*
 * Enumerates unrelated names and noncontiguous GPU instance numbers.
 */
struct dirent *
discovery_test_readdir(
	DIR *directory)
{
	static const char *names[] = { ".", "console", "gpu", "gpu0", "gpu-invalid", "gpu17", "gpu88", NULL };

	/* A reused entry buffer catches code which incorrectly retains readdir storage. */
	assert(directory == (DIR *)&directory_position);
	if (names[directory_position] == NULL)
		return NULL;
	memset(&directory_entry, 0, sizeof(directory_entry));
	strcpy(directory_entry.d_name, names[directory_position]);
	directory_position++;

	/* Succeeded: the next name remains valid until this traversal advances again. */
	return &directory_entry;
}

/*
 * Retires the directory token independently from any GPU namespace.
 */
int
discovery_test_closedir(
	DIR *directory)
{
	/* A completed traversal must close exactly the token it acquired. */
	assert(directory == (DIR *)&directory_position);

	/* Succeeded: GPU session lifetimes do not depend on directory storage. */
	return 0;
}

/*
 * Opens two independent compatible namespaces and one unsupported GPU node.
 */
VkResult
vulkan_context_open(
	struct vulkan_context *context,
	const char *path)
{
	struct discovery_peer *peer;
	unsigned index;
	unsigned node;
	int error;
	int match;

	/* An unrelated GPU implementation must not prevent discovery of compatible devices. */
	match = strcmp(path, "/dev/gpu88");
	if (match == 0)
		return VK_ERROR_INCOMPATIBLE_DRIVER;

	/* Noncontiguous numeric names must remain usable as ordinary dynamically registered devices. */
	node = 0U;
	match = strcmp(path, "/dev/gpu17");
	if (match == 0)
		node = 17U;
	else
		assert(strcmp(path, "/dev/gpu0") == 0);

	/* Each successful open owns an independent synchronization object and namespace record. */
	for (index = 0U; index < 4U; index++) {
		if (peers[index].context == NULL)
			break;
	}
	assert(index < 4U);
	peer = &peers[index];
	memset(peer, 0, sizeof(*peer));
	memset(context, 0, sizeof(*context));
	error = pthread_mutex_init(&context->mutex, NULL);
	assert(error == 0);
	peer->context = context;
	peer->node = node;
	context->fd = (int)node;
	context->capabilities = GPU_CAP_RESOURCE | GPU_CAP_CAPSET | GPU_CAP_BLOB |
	    GPU_CAP_TRANSFER | GPU_CAP_COMMAND | GPU_CAP_MAPPING | GPU_CAP_PRESENT;
	if (node == 17U)
		context->capabilities |= GPU_CAP_DISPLAY;
	opened++;

	/* Succeeded: only the second compatible GPU implements the new direct-display contract. */
	return VK_SUCCESS;
}

/*
 * Consumes one complete renderer namespace after instance rollback or destruction.
 */
VkResult
vulkan_context_close(
	struct vulkan_context *context)
{
	struct discovery_peer *peer;
	unsigned index;
	int error;

	/* Closing a namespace invalidates remaining private instance and physical identities together. */
	peer = discovery_peer_get(context);
	for (index = 0U; index < 80U; index++) {
		if (peer->devices[index].identity != 0U)
			native_devices--;
	}
	assert(context->queue_timelines == 0U);
	error = pthread_mutex_destroy(&context->mutex);
	assert(error == 0);
	memset(peer, 0, sizeof(*peer));
	closed++;

	/* Succeeded: a later open cannot reuse any live native identity from this session. */
	return VK_SUCCESS;
}

/*
 * Models the unrelated WSI teardown boundary without retaining any discovery object.
 */
void
vulkan_wsi_instance_finish(
	struct VkInstance_T *instance)
{
	/* This fixture creates no surfaces; physical devices must still exist at this boundary. */
	assert(instance != NULL);

	/* Succeeded: instance teardown may now consume its physical metadata and namespaces. */
	return;
}

/*
 * Observes logical-device teardown before native queue ownership is released.
 */
void
vulkan_wsi_device_finish(
	struct VkDevice_T *device)
{
	/* No swapchain exists in this fixture, but the real native device must still be live. */
	assert(device->object.context != NULL);

	/* Succeeded: ordinary native device destruction can proceed. */
	return;
}

/*
 * Parses actual production request envelopes and executes stateful native ownership.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *command,
	size_t capacity,
	struct vulkan_reader *reader)
{
	struct discovery_peer *peer;
	struct discovery_device *device;
	struct discovery_cursor cursor;
	struct vulkan_writer response;
	uint32_t opcode;
	uint64_t identity;
	int error;

	/* Independent command storage is serialized only at the native transport boundary. */
	peer = discovery_peer_get(context);
	error = pthread_mutex_lock(&context->mutex);
	assert(error == 0);

	/* A terminal context refuses further renderer access even during attempted native destruction. */
	if (context->error != VK_SUCCESS) {
		error = pthread_mutex_unlock(&context->mutex);
		assert(error == 0);
		return context->error;
	}

	/* The real transport allocates reply storage before native commands can have side effects. */
	memset(reader, 0, sizeof(*reader));
	reader->allocator = command->allocator;
	if (reader->allocator.has_callbacks != VK_FALSE)
		reader->data = vulkan_allocate(&reader->allocator, capacity, 16U, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	else
		reader->data = malloc(capacity);
	if (reader->data == NULL) {
		error = pthread_mutex_unlock(&context->mutex);
		assert(error == 0);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Only admitted transactions can create modeled native object ownership. */
	submissions++;
	cursor.data = command->data;
	cursor.bytes = command->bytes;
	cursor.offset = 0U;
	opcode = discovery_u32(&cursor);
	assert(discovery_u32(&cursor) == 1U);
	vulkan_writer_init(&response);
	vulkan_write_u32(&response, opcode);

	/* The peer distinguishes command namespaces and required output framing by actual opcode. */
	switch (opcode) {
	case VULKAN_OPCODE_vkCreateInstance:
		discovery_instance(peer, &cursor, &response);
		break;
	case VULKAN_OPCODE_vkEnumeratePhysicalDevices:
		discovery_enumerate(peer, &cursor, &response);
		break;
	case VULKAN_OPCODE_vkGetPhysicalDeviceProperties:
	case VULKAN_OPCODE_vkGetPhysicalDeviceFeatures:
	case VULKAN_OPCODE_vkGetPhysicalDeviceMemoryProperties:
	case VULKAN_OPCODE_vkGetPhysicalDeviceFormatProperties:
	case VULKAN_OPCODE_vkGetPhysicalDeviceImageFormatProperties:
		discovery_properties(peer, opcode, &cursor, &response);
		break;
	case VULKAN_OPCODE_vkGetPhysicalDeviceQueueFamilyProperties:
		discovery_queues(peer, &cursor, &response);
		break;
	case VULKAN_OPCODE_vkCreateDevice:
		discovery_create_device(peer, &cursor, &response);
		break;
	case VULKAN_OPCODE_vkGetDeviceQueue2:
		discovery_get_queue(peer, &cursor, &response);
		break;
	case VULKAN_OPCODE_vkDestroyDevice:
		identity = discovery_u64(&cursor);
		device = discovery_device_get(peer, identity);
		assert(discovery_u64(&cursor) == 0U);
		memset(device, 0, sizeof(*device));
		native_devices--;
		native_destroys++;
		break;
	default:
		assert(0);
	}

	/* Full request consumption catches wrong array cardinality, ordering and output placeholders. */
	assert(cursor.offset == cursor.bytes);
	discovery_copy_reply(command, &response, reader, capacity);
	vulkan_writer_finish(&response);
	error = pthread_mutex_unlock(&context->mutex);
	assert(error == 0);
	if (reader->data == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Succeeded: the production decoder owns a separately allocated complete response. */
	return VK_SUCCESS;
}

/*
 * Verifies discovery and logical-device lifetimes across differing real capability models.
 */
int
main(
	void)
{
	VkAllocationCallbacks allocator;
	VkAllocationCallbacks destroy_allocator;
	VkInstance instance;
	VkPhysicalDevice physical[2];

	/* Compatible destruction callbacks retain the allocation family with new user data. */
	memset(&allocator, 0, sizeof(allocator));
	allocator.pUserData = &allocation_user;
	allocator.pfnAllocation = discovery_allocate;
	allocator.pfnReallocation = discovery_reallocate;
	allocator.pfnFree = discovery_free;
	destroy_allocator = allocator;
	destroy_allocator.pUserData = &destruction_user;

	/* Public extension/version rejection must precede all device-directory and native activity. */
	discovery_check_extensions();
	instance = discovery_create_instance(&allocator);
	discovery_check_physical(instance, physical);
	discovery_check_devices(physical, &allocator, &destroy_allocator);
	vkDestroyInstance(instance, &destroy_allocator);
	assert(opened == closed);
	assert(native_devices == 0U);
	assert(allocated == freed);

	/* An unusable queue inventory must hide only its own GPU namespace. */
	discovery_check_queue_incompatibility(&allocator);

	/* Setup failure sweeps check callback inheritance and exact partial-namespace rollback. */
	discovery_check_failures(&allocator);
	assert(opened == closed);
	assert(native_devices == 0U);
	assert(allocated == freed);
	assert(destruction_frees != 0U);
	assert(queue_requests >= 68U);
	assert(native_destroys >= 4U);
	puts("libvulkan discovery/device: two GPU namespaces, capability filtering, stable queues, 63 timelines, real codec, callback rollback and native cleanup PASS");

	/* Succeeded: every modeled namespace and callback-owned object has retired. */
	return 0;
}

/* Reads a wire word independently of production scalar decoding. */
static uint32_t
discovery_u32(
	struct discovery_cursor *cursor)
{
	uint32_t number;
	unsigned index;

	/* Each access proves its entire interval before combining the little-endian bytes. */
	assert(cursor->offset <= cursor->bytes);
	assert(cursor->bytes - cursor->offset >= 4U);
	number = 0U;
	for (index = 0U; index < 4U; index++)
		number |= (uint32_t)cursor->data[cursor->offset + index] << (8U * index);
	cursor->offset += 4U;

	/* Succeeded: the next decoder access starts after this complete scalar. */
	return number;
}

/* Reads an identity without relying on native pointer width or structure padding. */
static uint64_t
discovery_u64(
	struct discovery_cursor *cursor)
{
	uint64_t low;
	uint64_t high;

	/* The two wire words preserve exact input order and 64-bit identity width. */
	low = discovery_u32(cursor);
	high = discovery_u32(cursor);

	/* Succeeded: the result can represent every renderer namespace identity. */
	return low | (high << 32);
}

/* Consumes an optional string while validating padding and its terminating byte. */
static void
discovery_string(
	struct discovery_cursor *cursor)
{
	uint64_t count;
	size_t padded;

	/* Zero represents an absent string rather than a missing required scalar. */
	count = discovery_u64(cursor);
	padded = ((size_t)count + 3U) & ~(size_t)3U;
	assert(padded <= cursor->bytes - cursor->offset);
	if (count != 0U)
		assert(cursor->data[cursor->offset + (size_t)count - 1U] == 0U);
	cursor->offset += padded;

	/* Succeeded: the following field begins after complete four-byte string padding. */
	return;
}

/* Checks the exact core structure prefix before consuming ordinary fields. */
static void
discovery_structure(
	struct discovery_cursor *cursor,
	uint32_t type)
{
	/* Standard core records carry no forwarded private host extension chain. */
	assert(discovery_u32(cursor) == type);
	assert(discovery_u64(cursor) == 0U);

	/* Succeeded: this cursor names the expected standard record. */
	return;
}

/* Resolves the modeled session associated with one actual context object. */
static struct discovery_peer *
discovery_peer_get(
	struct vulkan_context *context)
{
	unsigned index;

	/* No command may silently migrate from one GPU fd to another GPU namespace. */
	for (index = 0U; index < 4U; index++) {
		if (peers[index].context == context)
			return &peers[index];
	}
	assert(0);

	/* This unreachable fallback satisfies assertion-disabled builds. */
	return NULL;
}

/* Resolves a native device only within its creating renderer session. */
static struct discovery_device *
discovery_device_get(
	struct discovery_peer *peer,
	uint64_t identity)
{
	unsigned index;

	/* Native IDs are never accepted merely because another GPU owns a matching local address. */
	for (index = 0U; index < 80U; index++) {
		if (peer->devices[index].identity == identity)
			return &peer->devices[index];
	}
	assert(0);

	/* This unreachable fallback satisfies assertion-disabled builds. */
	return NULL;
}

/* Parses real application identity while enforcing the private native API version policy. */
static void
discovery_instance(
	struct discovery_peer *peer,
	struct discovery_cursor *cursor,
	struct vulkan_writer *response)
{
	/* WSI extension selection belongs to the guest and must not be forwarded as host surfaces. */
	assert(discovery_u64(cursor) == 1U);
	discovery_structure(cursor, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
	assert(discovery_u32(cursor) == 0U);
	assert(discovery_u64(cursor) == 1U);
	discovery_structure(cursor, VK_STRUCTURE_TYPE_APPLICATION_INFO);
	discovery_string(cursor);
	assert(discovery_u32(cursor) == 71U);
	discovery_string(cursor);
	assert(discovery_u32(cursor) == 19U);
	assert(discovery_u32(cursor) == VK_MAKE_VERSION(1, 1, 0));
	assert(discovery_u32(cursor) == 0U);
	assert(discovery_u64(cursor) == 0U);
	assert(discovery_u32(cursor) == 0U);
	assert(discovery_u64(cursor) == 0U);
	assert(discovery_u64(cursor) == 0U);
	assert(discovery_u64(cursor) == 1U);
	assert(peer->instance == 0U);
	peer->instance = discovery_u64(cursor);
	vulkan_write_u32(response, VK_SUCCESS);
	vulkan_write_u64(response, 1U);
	vulkan_write_u64(response, peer->instance);

	/* Succeeded: each GPU has a distinct actual renderer instance identity. */
	return;
}

/* Maps one native physical device behind an independently reserved public identity. */
static void
discovery_enumerate(
	struct discovery_peer *peer,
	struct discovery_cursor *cursor,
	struct vulkan_writer *response)
{
	uint32_t count;
	uint64_t array_count;

	/* Count queries must supply no output identity or uninitialized input count. */
	assert(discovery_u64(cursor) == peer->instance);
	assert(discovery_u64(cursor) == 1U);
	count = discovery_u32(cursor);
	array_count = discovery_u64(cursor);
	vulkan_write_u32(response, VK_SUCCESS);
	vulkan_write_u64(response, 1U);
	vulkan_write_u32(response, 1U);

	/* This peer exposes one physical device per native namespace and echoes requested output IDs. */
	if (array_count == 0U) {
		assert(count == 0U);
		vulkan_write_u64(response, 0U);
	} else {
		assert(count == 1U && array_count == 1U);
		assert(peer->physical == 0U);
		peer->physical = discovery_u64(cursor);
		vulkan_write_u64(response, 1U);
		if (bad_physical_echo != 0U) {
			bad_physical_echo = 0U;
			vulkan_write_u64(response, peer->physical + 123U);
		} else {
			vulkan_write_u64(response, peer->physical);
		}
	}

	/* Succeeded: the public composite list must retain the namespace that owns this identity. */
	return;
}

/* Supplies complete physical snapshots with deliberately different native and exposed guarantees. */
static void
discovery_properties(
	struct discovery_peer *peer,
	uint32_t opcode,
	struct discovery_cursor *cursor,
	struct vulkan_writer *response)
{
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceMemoryProperties memory;
	VkFormatProperties format;
	VkImageFormatProperties image;
	uint32_t requested_format;
	unsigned index;

	/* Every query must use the physical identity reserved within this exact GPU namespace. */
	assert(discovery_u64(cursor) == peer->physical);
	requested_format = 0U;
	if (opcode == VULKAN_OPCODE_vkGetPhysicalDeviceFormatProperties) {
		requested_format = discovery_u32(cursor);
		assert(requested_format == VK_FORMAT_R8G8B8A8_UNORM);
	} else if (opcode == VULKAN_OPCODE_vkGetPhysicalDeviceImageFormatProperties) {
		requested_format = discovery_u32(cursor);
		assert(discovery_u32(cursor) == VK_IMAGE_TYPE_2D);
		assert(discovery_u32(cursor) == VK_IMAGE_TILING_OPTIMAL);
		assert(discovery_u32(cursor) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		assert(discovery_u32(cursor) == 0U);
	}
	assert(discovery_u64(cursor) == 1U);

	/* Result-bearing format queries keep unsupported requests distinct from malformed replies. */
	if (opcode == VULKAN_OPCODE_vkGetPhysicalDeviceImageFormatProperties) {
		if (requested_format == VK_FORMAT_UNDEFINED) {
			vulkan_write_u32(response, (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED);
			return;
		}
		vulkan_write_u32(response, VK_SUCCESS);
	}
	vulkan_write_u64(response, 1U);

	/* Typed fixed payloads exercise integration; the fixture independently checks their request framing. */
	switch (opcode) {
	case VULKAN_OPCODE_vkGetPhysicalDeviceProperties:
		memset(&properties, 0, sizeof(properties));
		properties.apiVersion = VK_MAKE_VERSION(1, 3, 0);
		properties.driverVersion = 321U;
		properties.vendorID = 0x8086U;
		properties.deviceID = peer->node + 123U;
		properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
		strcpy(properties.deviceName, "Independent discovery peer");
		properties.limits.maxImageDimension2D = 8192U;
		properties.limits.maxBoundDescriptorSets = 8U;
		properties.limits.minMemoryMapAlignment = 8192U;
		properties.limits.nonCoherentAtomSize = 256U;
		vulkan_encode_VkPhysicalDeviceProperties(response, &properties);
		break;
	case VULKAN_OPCODE_vkGetPhysicalDeviceFeatures:
		memset(&features, 0, sizeof(features));
		features.robustBufferAccess = VK_TRUE;
		features.shaderInt64 = VK_TRUE;
		features.samplerAnisotropy = VK_TRUE;
		vulkan_encode_VkPhysicalDeviceFeatures(response, &features);
		break;
	case VULKAN_OPCODE_vkGetPhysicalDeviceMemoryProperties:
		assert(discovery_u64(cursor) == VK_MAX_MEMORY_TYPES);
		assert(discovery_u64(cursor) == VK_MAX_MEMORY_HEAPS);
		memset(&memory, 0, sizeof(memory));
		memory.memoryTypeCount = 4U;
		memory.memoryHeapCount = 2U;
		memory.memoryHeaps[0].size = UINT64_C(1) << 30;
		memory.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
		memory.memoryHeaps[1].size = UINT64_C(2) << 30;
		memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		memory.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		memory.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (index = 1U; index < 3U; index++)
			memory.memoryTypes[index].heapIndex = 1U;
		vulkan_encode_VkPhysicalDeviceMemoryProperties(response, &memory);
		break;
	case VULKAN_OPCODE_vkGetPhysicalDeviceFormatProperties:
		memset(&format, 0, sizeof(format));
		format.optimalTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
		format.bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
		vulkan_encode_VkFormatProperties(response, &format);
		break;
	case VULKAN_OPCODE_vkGetPhysicalDeviceImageFormatProperties:
		memset(&image, 0, sizeof(image));
		image.maxExtent.width = 8192U;
		image.maxExtent.height = 4096U;
		image.maxExtent.depth = 1U;
		image.maxMipLevels = 14U;
		image.maxArrayLayers = 256U;
		image.sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
		image.maxResourceSize = UINT64_C(1) << 32;
		vulkan_encode_VkImageFormatProperties(response, &image);
		break;
	default:
		assert(0);
	}

	/* Succeeded: the API must preserve useful fields while applying only its documented local filters. */
	return;
}

/* Returns three real native queue-family indices with differing roles and capacities. */
static void
discovery_queues(
	struct discovery_peer *peer,
	struct discovery_cursor *cursor,
	struct vulkan_writer *response)
{
	uint32_t count;
	uint32_t total;
	uint32_t queues;
	uint64_t array_count;
	unsigned index;

	/* Primitive-only output records need array count placeholders but no record input bytes. */
	assert(discovery_u64(cursor) == peer->physical);
	assert(discovery_u64(cursor) == 1U);
	count = discovery_u32(cursor);
	array_count = discovery_u64(cursor);

	/* Only the selected GPU changes its native capability shape in rejection scenarios. */
	total = 3U;
	if (peer->node == 17U && native_family_count != 0U)
		total = native_family_count;

	/* Count-only discovery carries no output records or caller-owned record placeholders. */
	vulkan_write_u64(response, 1U);
	vulkan_write_u32(response, total);
	if (array_count == 0U) {
		assert(count == 0U);
		vulkan_write_u64(response, 0U);
		return;
	}

	/* Full discovery preserves all native indices before the library applies its queue budget. */
	assert(count == total && array_count == total);
	vulkan_write_u64(response, total);
	for (index = 0U; index < total; index++) {
		/* Native queue capacities deliberately exceed the complete renderer timeline budget. */
		if (index == 0U) {
			vulkan_write_u32(response, VK_QUEUE_TRANSFER_BIT);
			queues = 64U;
		} else if (index == 1U) {
			vulkan_write_u32(response, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT);
			queues = 2U;
		} else {
			vulkan_write_u32(response, VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT);
			queues = 3U;
		}

		/* A zero native queue family cannot be made valid by reserving a synthetic queue. */
		if (peer->node == 17U &&
		    native_zero_queue != 0U &&
		    index == 1U)
			queues = 0U;

		/* Ordinary queue properties retain their timestamp and transfer granularity metadata. */
		vulkan_write_u32(response, queues);
		vulkan_write_u32(response, 48U);
		vulkan_write_u32(response, 1U);
		vulkan_write_u32(response, 1U);
		vulkan_write_u32(response, 1U);
	}

	/* Succeeded: logical-device creation can request a later family without remapping its index. */
	return;
}

/* Creates a native logical device after independently parsing standard queue and feature inputs. */
static void
discovery_create_device(
	struct discovery_peer *peer,
	struct discovery_cursor *cursor,
	struct vulkan_writer *response)
{
	struct discovery_device pending;
	uint32_t create_count;
	uint32_t family;
	uint32_t count;
	uint64_t enabled_features;
	uint32_t index;
	uint32_t queue;
	unsigned slot;

	/* Local WSI names must be stripped while application queue roles and priorities remain unchanged. */
	memset(&pending, 0, sizeof(pending));
	assert(discovery_u64(cursor) == peer->physical);
	assert(discovery_u64(cursor) == 1U);
	discovery_structure(cursor, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
	assert(discovery_u32(cursor) == 0U);
	create_count = discovery_u32(cursor);
	assert(discovery_u64(cursor) == create_count);
	for (index = 0U; index < create_count; index++) {
		discovery_structure(cursor, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
		assert(discovery_u32(cursor) == 0U);
		family = discovery_u32(cursor);
		count = discovery_u32(cursor);
		assert(family < 3U && count != 0U);
		assert(pending.family_counts[family] == 0U);
		pending.family_counts[family] = count;
		assert(discovery_u64(cursor) == count);
		for (queue = 0U; queue < count; queue++)
			assert(discovery_u32(cursor) == 0x3f000000U);
	}
	assert(discovery_u32(cursor) == 0U);
	assert(discovery_u64(cursor) == 0U);
	assert(discovery_u32(cursor) == 0U);
	assert(discovery_u64(cursor) == 0U);
	enabled_features = discovery_u64(cursor);
	if (enabled_features != 0U) {
		assert(enabled_features == 1U);
		for (index = 0U; index < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); index++)
			assert(discovery_u32(cursor) <= 1U);
	}
	assert(discovery_u64(cursor) == 0U);
	assert(discovery_u64(cursor) == 1U);
	pending.identity = discovery_u64(cursor);

	/* A native allocation refusal creates no device even though local queue slots were reserved. */
	if (fail_device != 0U) {
		fail_device = 0U;
		vulkan_write_u32(response, (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY);
		return;
	}
	for (slot = 0U; slot < 80U; slot++) {
		if (peer->devices[slot].identity == 0U)
			break;
	}
	assert(slot < 80U);
	peer->devices[slot] = pending;
	native_devices++;
	vulkan_write_u32(response, VK_SUCCESS);
	vulkan_write_u64(response, 1U);
	vulkan_write_u64(response, pending.identity);

	/* Succeeded: native queue identities can now be assigned exactly once. */
	return;
}

/* Binds each native queue to one unique renderer timeline and immutable public identity. */
static void
discovery_get_queue(
	struct discovery_peer *peer,
	struct discovery_cursor *cursor,
	struct vulkan_writer *response)
{
	struct discovery_device *device;
	uint64_t identity;
	uint64_t queue_identity;
	uint64_t used;
	uint32_t timeline;
	uint32_t family;
	uint32_t index;
	unsigned other;

	/* The pinned Venus protocol requires GetDeviceQueue2 plus its private timeline structure. */
	identity = discovery_u64(cursor);
	device = discovery_device_get(peer, identity);
	assert(discovery_u64(cursor) == 1U);
	assert(discovery_u32(cursor) == 1000145003U);
	assert(discovery_u64(cursor) == 1U);
	assert(discovery_u32(cursor) == 1000384005U);
	assert(discovery_u64(cursor) == 0U);
	timeline = discovery_u32(cursor);
	assert(discovery_u32(cursor) == 0U);
	family = discovery_u32(cursor);
	index = discovery_u32(cursor);
	assert(discovery_u64(cursor) == 1U);
	queue_identity = discovery_u64(cursor);
	assert(family < 3U && index < device->family_counts[family]);
	assert(timeline >= 1U && timeline <= 63U);

	/* Every live queue timeline is unique across all logical devices in this same namespace. */
	used = 0U;
	for (other = 0U; other < 80U; other++)
		used |= peer->devices[other].timelines;
	assert((used & (UINT64_C(1) << timeline)) == 0U);
	assert((peer->context->queue_timelines & (UINT64_C(1) << timeline)) != 0U);
	device->timelines |= UINT64_C(1) << timeline;
	queue_requests++;
	vulkan_write_u64(response, 1U);
	if (fail_queue != 0U) {
		fail_queue = 0U;
		vulkan_write_u64(response, queue_identity + 19U);
	} else {
		vulkan_write_u64(response, queue_identity);
	}

	/* Succeeded: repeated public GetDeviceQueue calls must reuse cached local handles. */
	return;
}

/* Transfers a complete peer reply to the ordinary reader with the correct callback family. */
static void
discovery_copy_reply(
	const struct vulkan_writer *command,
	struct vulkan_writer *response,
	struct vulkan_reader *reader,
	size_t capacity)
{
	/* The peer never writes beyond the response extent allocated by the API implementation. */
	assert(response->error == VK_SUCCESS);
	assert(response->bytes <= capacity);
	(void)command;
	assert(reader->data != NULL);
	memcpy(reader->data, response->data, response->bytes);
	reader->bytes = response->bytes;

	/* Succeeded: releasing the production reader cannot retire peer-owned command storage. */
	return;
}

/* Allocates callback storage with deterministic failure accounting across every API scope. */
static void *
discovery_allocate(
	void *user,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *pointer;
	int error;

	/* Inherited policies remain valid for instance, device, object and command lifetimes. */
	assert(user == &allocation_user || user == &destruction_user);
	assert(scope >= VK_SYSTEM_ALLOCATION_SCOPE_COMMAND && scope <= VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	allocation_attempt++;
	if (allocation_attempt == fail_allocation)
		return NULL;
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);
	pointer = NULL;
	error = posix_memalign(&pointer, alignment, bytes);
	assert(error == 0);
	allocated++;

	/* Succeeded: compatible callback destruction must eventually consume this exact block. */
	return pointer;
}

/* Rejects an unused reallocation path while providing a complete standard callback set. */
static void *
discovery_reallocate(
	void *user,
	void *original,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	/* Production command growth uses allocate-copy-free with the same declared callback policy. */
	(void)user;
	(void)original;
	(void)bytes;
	(void)alignment;
	(void)scope;
	assert(0);

	/* This unreachable fallback satisfies the standard callback signature. */
	return NULL;
}

/* Retires only callback-owned allocations and records current destruction user data. */
static void
discovery_free(
	void *user,
	void *pointer)
{
	/* Null callback frees carry no allocation lifetime. */
	assert(user == &allocation_user || user == &destruction_user);
	if (pointer == NULL)
		return;
	if (user == &destruction_user)
		destruction_frees++;
	freed++;
	free(pointer);

	/* Succeeded: this individual callback allocation no longer survives its owning API object. */
	return;
}

/* Constructs a standard 1.0 instance with local direct-display extensions enabled. */
static VkInstance
discovery_create_instance(
	const VkAllocationCallbacks *allocator)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	const char *extensions[2];
	VkInstance instance;
	VkResult error;

	/* Application identity is preserved while native transport privately raises its own API version. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "discovery-fixture";
	application.applicationVersion = 71U;
	application.pEngineName = "independent-peer";
	application.engineVersion = 19U;
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_DISPLAY_EXTENSION_NAME;
	info.enabledExtensionCount = 2U;
	info.ppEnabledExtensionNames = extensions;
	instance = VK_NULL_HANDLE;
	error = vkCreateInstance(&info, allocator, &instance);
	assert(error == VK_SUCCESS && instance != VK_NULL_HANDLE);

	/* Succeeded: the instance owns every compatible GPU namespace found in the directory. */
	return instance;
}

/* Checks stable physical snapshots and truthful per-GPU local capability boundaries. */
static void
discovery_check_physical(
	VkInstance instance,
	VkPhysicalDevice *physical)
{
	VkPhysicalDevice short_list;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceMemoryProperties memory;
	VkQueueFamilyProperties queues[3];
	VkExtensionProperties extensions[2];
	VkFormatProperties format;
	VkImageFormatProperties image;
	uint32_t count;
	unsigned index;
	VkResult error;

	/* Count and truncated enumeration preserve both available count and stable physical identities. */
	count = UINT32_MAX;
	error = vkEnumeratePhysicalDevices(instance, &count, NULL);
	assert(error == VK_SUCCESS && count == 2U);
	count = 1U;
	error = vkEnumeratePhysicalDevices(instance, &count, &short_list);
	assert(error == VK_INCOMPLETE && count == 1U);
	count = 2U;
	error = vkEnumeratePhysicalDevices(instance, &count, physical);
	assert(error == VK_SUCCESS && physical[0] == short_list);
	assert(physical[0] != physical[1]);
	assert(vulkan_physical_device(physical[0])->object.context != vulkan_physical_device(physical[1])->object.context);

	/* Complete fixed fields survive decoding while local public version and map alignment stay truthful. */
	for (index = 0U; index < 2U; index++) {
		vkGetPhysicalDeviceProperties(physical[index], &properties);
		assert(properties.apiVersion == VK_API_VERSION_1_0);
		assert(properties.driverVersion == 321U);
		assert(properties.limits.maxImageDimension2D == 8192U);
		assert(properties.limits.minMemoryMapAlignment == 64U);
		assert(properties.limits.nonCoherentAtomSize == 256U);
		vkGetPhysicalDeviceFeatures(physical[index], &features);
		assert(features.robustBufferAccess == VK_TRUE && features.shaderInt64 == VK_TRUE);
		assert(features.wideLines == VK_FALSE);
		vkGetPhysicalDeviceMemoryProperties(physical[index], &memory);
		assert(memory.memoryTypeCount == 4U && memory.memoryHeapCount == 2U);
		assert(memory.memoryTypes[1].heapIndex == 1U);
		assert(memory.memoryTypes[1].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		assert(memory.memoryTypes[2].propertyFlags == 0U);
		assert(memory.memoryTypes[2].heapIndex == 1U);
		assert(memory.memoryTypes[3].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		count = UINT32_MAX;
		vkGetPhysicalDeviceQueueFamilyProperties(physical[index], &count, NULL);
		assert(count == 3U);
		count = 1U;
		vkGetPhysicalDeviceQueueFamilyProperties(physical[index], &count, queues);
		assert(count == 1U && queues[0].queueCount == 61U);
		count = 3U;
		vkGetPhysicalDeviceQueueFamilyProperties(physical[index], &count, queues);
		assert(count == 3U && queues[2].queueCount == 1U);
		assert(queues[0].queueCount + queues[1].queueCount + queues[2].queueCount == 63U);
		assert(queues[1].queueCount == 1U);
		assert(queues[1].queueFlags & VK_QUEUE_GRAPHICS_BIT);
	}

	/* Legacy copied presentation cannot substitute for the new native FIFO and display ownership API. */
	count = 2U;
	error = vkEnumerateDeviceExtensionProperties(physical[0], NULL, &count, extensions);
	assert(error == VK_SUCCESS && count == 0U);
	count = 1U;
	error = vkEnumerateDeviceExtensionProperties(physical[1], NULL, &count, extensions);
	assert(error == VK_INCOMPLETE && count == 1U);
	assert(strcmp(extensions[0].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0);
	count = 2U;
	error = vkEnumerateDeviceExtensionProperties(physical[1], NULL, &count, extensions);
	assert(error == VK_SUCCESS && count == 2U);

	/* Arbitrary format requests reach the associated physical namespace with all standard parameters. */
	vkGetPhysicalDeviceFormatProperties(physical[1], VK_FORMAT_R8G8B8A8_UNORM, &format);
	assert(format.optimalTilingFeatures == VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
	assert(format.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
	error = vkGetPhysicalDeviceImageFormatProperties(physical[1], VK_FORMAT_R8G8B8A8_UNORM,
	    VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0U, &image);
	assert(error == VK_SUCCESS && image.maxResourceSize == (UINT64_C(1) << 32));
	memset(&image, 0xa5, sizeof(image));
	error = vkGetPhysicalDeviceImageFormatProperties(physical[1], VK_FORMAT_UNDEFINED,
	    VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0U, &image);
	assert(error == VK_ERROR_FORMAT_NOT_SUPPORTED);
	assert(image.maxExtent.width == 0xa5a5a5a5U);

	/* Succeeded: each returned capability belongs to the GPU that actually implements it. */
	return;
}

/* Exercises logical-device queues, native timeline exhaustion and exact rollback ownership. */
static void
discovery_check_devices(
	VkPhysicalDevice *physical,
	const VkAllocationCallbacks *allocator,
	const VkAllocationCallbacks *destroy_allocator)
{
	VkDeviceCreateInfo info;
	VkDeviceQueueCreateInfo queues[3];
	VkPhysicalDeviceFeatures features;
	VkDevice device;
	VkDevice other;
	VkDevice refused;
	VkQueue queue;
	VkQueue repeated;
	const char *extensions[2];
	float priorities[64];
	struct vulkan_context *context;
	uint64_t timelines;
	unsigned index;
	unsigned before;
	unsigned position;
	VkResult error;

	/* Requests later native family indices in deliberately nonnumeric creation order. */
	for (index = 0U; index < 64U; index++)
		priorities[index] = 0.5f;
	memset(queues, 0, sizeof(queues));
	queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queues[0].queueFamilyIndex = 2U;
	queues[0].queueCount = 1U;
	queues[0].pQueuePriorities = priorities;
	queues[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queues[1].queueFamilyIndex = 1U;
	queues[1].queueCount = 1U;
	queues[1].pQueuePriorities = priorities;
	memset(&features, 0, sizeof(features));
	features.shaderInt64 = VK_TRUE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 2U;
	info.pQueueCreateInfos = queues;
	info.pEnabledFeatures = &features;
	extensions[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	extensions[1] = VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME;
	info.enabledExtensionCount = 2U;
	info.ppEnabledExtensionNames = extensions;
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_SUCCESS);
	before = submissions;
	vkGetDeviceQueue(device, 2U, 0U, &queue);
	vkGetDeviceQueue(device, 2U, 0U, &repeated);
	assert(queue == repeated);
	assert(vulkan_queue(queue)->family == 2U && vulkan_queue(queue)->index == 0U);
	vkGetDeviceQueue(device, 1U, 0U, &repeated);
	assert(queue != repeated);
	assert(submissions == before);
	vkDestroyDevice(device, destroy_allocator);
	context = vulkan_physical_device(physical[1])->object.context;
	assert(context->queue_timelines == 0U);

	/* Unsupported features and local extensions fail before native device creation. */
	before = submissions;
	features.wideLines = VK_TRUE;
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_ERROR_FEATURE_NOT_PRESENT);
	features.wideLines = VK_FALSE;
	error = vkCreateDevice(physical[0], &info, allocator, &device);
	assert(error == VK_ERROR_EXTENSION_NOT_PRESENT);
	assert(submissions == before);
	info.enabledExtensionCount = 0U;

	/* The renderer has 63 nonzero timelines, with independent capacity in the other GPU session. */
	queues[0].queueFamilyIndex = 0U;
	queues[0].queueCount = 61U;
	queues[2].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queues[2].queueFamilyIndex = 2U;
	queues[2].queueCount = 1U;
	queues[2].pQueuePriorities = priorities;
	info.queueCreateInfoCount = 3U;
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_SUCCESS);
	assert(context->queue_timelines == (UINT64_MAX & ~UINT64_C(1)));
	before = submissions;
	queues[0].queueCount = 1U;
	info.queueCreateInfoCount = 1U;
	refused = VK_NULL_HANDLE;
	error = vkCreateDevice(physical[1], &info, allocator, &refused);
	assert(error == VK_ERROR_OUT_OF_DEVICE_MEMORY && refused == VK_NULL_HANDLE);
	assert(submissions == before);
	assert(context->queue_timelines == (UINT64_MAX & ~UINT64_C(1)));
	error = vkCreateDevice(physical[0], &info, allocator, &other);
	assert(error == VK_SUCCESS);
	vkDestroyDevice(other, destroy_allocator);
	vkDestroyDevice(device, destroy_allocator);
	assert(context->queue_timelines == 0U);

	/* A released timeline can be reused only after the previous native device has retired. */
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_SUCCESS);
	vkGetDeviceQueue(device, 0U, 0U, &queue);
	assert(vulkan_queue(queue)->timeline_index == 1U);
	vkDestroyDevice(device, destroy_allocator);

	/* A native device allocation failure returns every local queue reservation before retry. */
	fail_device = 1U;
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(context->queue_timelines == 0U);

	/* Local preflight failures leave existing physical and namespace ownership unchanged. */
	timelines = context->queue_timelines;
	for (position = 1U; position <= 6U; position++) {
		allocation_attempt = 0U;
		fail_allocation = position;
		device = VK_NULL_HANDLE;
		error = vkCreateDevice(physical[1], &info, allocator, &device);
		assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
		assert(device == VK_NULL_HANDLE);
		assert(context->queue_timelines == timelines);
		assert(native_devices == 0U);
	}
	fail_allocation = 0U;

	/* A malformed queue echo makes its namespace terminal rather than reusing unknown native ownership. */
	before = native_destroys;
	fail_queue = 1U;
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_ERROR_DEVICE_LOST);
	assert(context->queue_timelines == 0U);
	assert(context->error == VK_ERROR_DEVICE_LOST);
	assert(native_destroys == before);
	assert(native_devices == 1U);

	/* Further native creation is refused until instance teardown consumes the entire lost namespace. */
	error = vkCreateDevice(physical[1], &info, allocator, &device);
	assert(error == VK_ERROR_DEVICE_LOST);
	assert(context->queue_timelines == 0U);
	assert(native_devices == 1U);

	/* Succeeded: logical-device failure never consumes another device's queues or namespace. */
	return;
}

/* Sweeps instance setup failures and malformed physical identity publication. */
static void
discovery_check_failures(
	const VkAllocationCallbacks *allocator)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	VkInstance instance;
	unsigned position;
	VkResult error;

	/* The peer checks application identity even during partial creation rollback. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.applicationVersion = 71U;
	application.engineVersion = 19U;
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;

	/* Every callback failure in either namespace must return all previously acquired ownership. */
	for (position = 1U; position <= 38U; position++) {
		allocation_attempt = 0U;
		fail_allocation = position;
		instance = VK_NULL_HANDLE;
		error = vkCreateInstance(&info, allocator, &instance);
		if (error == VK_SUCCESS)
			vkDestroyInstance(instance, allocator);
		else
			assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
		assert(opened == closed);
		assert(native_devices == 0U);
		assert(allocated == freed);
	}
	fail_allocation = 0U;

	/* A foreign renderer physical identity must not become a usable composite public handle. */
	bad_physical_echo = 1U;
	instance = VK_NULL_HANDLE;
	error = vkCreateInstance(&info, allocator, &instance);
	assert(error == VK_ERROR_DEVICE_LOST);
	assert(instance == VK_NULL_HANDLE);
	assert(opened == closed);

	/* Succeeded: partial discovery never leaks a compatible namespace or an inherited allocation. */
	return;
}

/* Checks the exact advertised extension set and rejects unsupported public API versions early. */
static void
discovery_check_extensions(
	void)
{
	VkExtensionProperties properties[2];
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	VkInstance instance;
	const char *extension;
	uint32_t count;
	unsigned before;
	VkResult error;

	/* The implementation advertises precisely the two selected standard instance extensions. */
	count = 0U;
	error = vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
	assert(error == VK_SUCCESS && count == 2U);
	count = 1U;
	error = vkEnumerateInstanceExtensionProperties(NULL, &count, properties);
	assert(error == VK_INCOMPLETE && count == 1U);
	count = 2U;
	error = vkEnumerateInstanceExtensionProperties(NULL, &count, properties);
	assert(error == VK_SUCCESS && count == 2U);
	assert(strcmp(properties[0].extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0);
	assert(strcmp(properties[1].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0);
	error = vkEnumerateInstanceExtensionProperties("missing-layer", &count, properties);
	assert(error == VK_ERROR_LAYER_NOT_PRESENT);

	/* A public 1.1 request cannot silently succeed against the exposed 1.0 implementation. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.apiVersion = VK_MAKE_VERSION(1, 1, 0);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	before = opened;
	error = vkCreateInstance(&info, NULL, &instance);
	assert(error == VK_ERROR_INCOMPATIBLE_DRIVER);
	assert(opened == before);

	/* Display requires surface support in the same public instance. */
	application.apiVersion = VK_API_VERSION_1_0;
	extension = VK_KHR_DISPLAY_EXTENSION_NAME;
	info.enabledExtensionCount = 1U;
	info.ppEnabledExtensionNames = &extension;
	error = vkCreateInstance(&info, NULL, &instance);
	assert(error == VK_ERROR_EXTENSION_NOT_PRESENT);
	assert(opened == before);

	/* Succeeded: unsupported version and dependency promises never reach the native transport. */
	return;
}
/* Excludes incompatible queue inventories without discarding another GPU's public identity. */
static void
discovery_check_queue_incompatibility(
	const VkAllocationCallbacks *allocator)
{
	VkInstance instance;
	VkPhysicalDevice physical;
	VkQueueFamilyProperties queues[3];
	uint32_t count;
	unsigned scenario;
	unsigned before_opened;
	unsigned before_closed;
	VkResult error;

	/* Both excessive families and a zero-capacity native family make only GPU17 unusable. */
	for (scenario = 0U; scenario < 2U; scenario++) {
		/* Selects one malformed capacity shape without changing the first namespace. */
		native_family_count = 0U;
		native_zero_queue = 0U;
		if (scenario == 0U)
			native_family_count = 64U;
		else
			native_zero_queue = 1U;

		/* A complete first GPU survives while the second native namespace retires immediately. */
		before_opened = opened;
		before_closed = closed;
		instance = discovery_create_instance(allocator);
		assert(opened == before_opened + 2U);
		assert(closed == before_closed + 1U);
		count = 1U;
		error = vkEnumeratePhysicalDevices(instance, &count, &physical);
		assert(error == VK_SUCCESS && count == 1U);
		assert(vulkan_physical_device(physical)->object.context->fd == 0);

		/* The surviving namespace retains its own native family indices and realizable queue budget. */
		count = 3U;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
		assert(count == 3U);
		assert(queues[0].queueCount == 61U);
		assert(queues[1].queueCount == 1U);
		assert(queues[2].queueCount == 1U);
		vkDestroyInstance(instance, allocator);
		assert(opened == closed);
		assert(native_devices == 0U);
		assert(allocated == freed);
	}

	/* Later failure sweeps use the ordinary two-compatible-GPU inventory again. */
	native_family_count = 0U;
	native_zero_queue = 0U;

	/* Succeeded: unsupported queue shape never disables another compatible GPU. */
	return;
}
