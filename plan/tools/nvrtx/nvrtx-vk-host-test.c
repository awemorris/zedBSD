/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the nvrtx Vulkan executor's object registry, session and
 * transport (ws037-p007 stage D2).  The client side is libvulkan's own
 * wire writer; two arrays stand in for the session's blobs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"
#include "userland/desktop/libvulkan/internal.h"

/* A body only this test sends: it echoes one word, so forwarding and the external copy show. */
#define TEST_BODY_OPCODE	0x10017U

static unsigned failures;
static unsigned checks;
static uint32_t reply_words[512];
static uint32_t external_words[64];
static unsigned dispatched;
static int mutate_external;
static unsigned allocations;
static unsigned destroyed;

static void check(int ok, const char *what);
static void encode_selector(struct vulkan_writer *writer, uint64_t capacity);
static void encode_trailer(struct vulkan_writer *writer, uint64_t offset);
static void encode_external(struct vulkan_writer *writer, uint64_t bytes, uint32_t flags);
static void encode_body(struct vulkan_writer *writer, uint32_t bits);
static int dispatch_body(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_payload(struct nvrtx_vk_session *session, void *payload);
static void test_objects(void);
static void test_stream(void);
static void test_device(void);
static void test_push(void);
static int push_value(const uint8_t *push, size_t bytes, uint32_t method, uint32_t *value);

void *
kern_malloc(size_t bytes)
{
	allocations++;
	return malloc(bytes);
}

void *
kern_calloc(size_t count, size_t bytes)
{
	allocations++;
	return calloc(count, bytes);
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		allocations--;
	free(pointer);
}

/* The two blobs: 1 the reply array, 2 the external-stream array. */
int
nvrtx_vk_blob(void *owner, uint32_t resource_id, uint8_t **cpu, uint64_t *bytes)
{
	(void)owner;
	if (resource_id == 1U) {
		*cpu = (uint8_t *)reply_words;
		*bytes = sizeof(reply_words);
		return 0;
	}
	if (resource_id == 2U) {
		*cpu = (uint8_t *)external_words;
		*bytes = sizeof(external_words);
		return 0;
	}
	return ENOENT;
}

uint32_t
nvrtx_vk_device_id(void *owner)
{
	(void)owner;
	return 0x1f02U;
}

uint64_t
nvrtx_vk_heap_bytes(void *owner)
{
	(void)owner;
	return 240ULL << 20;
}

uint32_t
nvrtx_vk_shader_model(void *owner)
{
	(void)owner;
	return 75U;
}

uint32_t
nvrtx_vk_threed_class(void *owner)
{
	(void)owner;
	return 0xc597U;
}

/* The node's other parts the dispatcher's later handlers reach (the D3 test covers them). */
void
kern_logf(const char *format, ...)
{
	(void)format;
}

void
nvrtx_vk_memory_detach(void *owner, void *backing, uint64_t va, uint64_t bytes)
{
	(void)owner;
	(void)backing;
	(void)va;
	(void)bytes;
}

int
nvrtx_vk_memory_kind(void *owner, void *backing, uint64_t offset, uint64_t va, uint64_t bytes, uint32_t kind)
{
	(void)owner;
	(void)backing;
	(void)offset;
	(void)va;
	(void)bytes;
	(void)kind;
	return 0;
}

/* The host's storage keeps no shape: every image binds. */
int
nvrtx_vk_memory_bind_image(void *backing, bool tiled, uint32_t pitch, uint32_t width, uint32_t height, uint64_t offset)
{
	(void)backing;
	(void)tiled;
	(void)pitch;
	(void)width;
	(void)height;
	(void)offset;
	return 0;
}

int
nvrtx_vk_memory_import(void *owner, uint32_t resource_id, uint64_t bytes, uint8_t **cpu, uint64_t *va, void **backing)
{
	(void)owner;
	(void)resource_id;
	(void)bytes;
	(void)cpu;
	(void)va;
	(void)backing;
	return ENOENT;
}

int
nvrtx_vk_memory_cpu(void *owner, void *backing, uint8_t **cpu)
{
	(void)owner;
	(void)backing;
	(void)cpu;
	return ENOSPC;
}

int
nvrtx_vk_gpu_alloc(void *owner, uint64_t bytes, void **token, uint8_t **cpu, uint64_t *va)
{
	(void)owner;
	(void)bytes;
	(void)token;
	(void)cpu;
	(void)va;
	return ENOMEM;
}

int
nvrtx_vk_gpu_alloc_system(void *owner, uint64_t bytes, void **token, uint8_t **cpu, uint64_t *va)
{
	(void)owner;
	(void)bytes;
	(void)token;
	(void)cpu;
	(void)va;
	return ENOMEM;
}

void
nvrtx_vk_gpu_free(void *owner, void *token)
{
	(void)owner;
	(void)token;
}

int
nvrtx_vk_gpu_submit(void *owner, uint64_t va, uint32_t bytes, struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	(void)va;
	(void)bytes;
	ticket->fence = 0U;
	ticket->submitted_us = 0U;
	return EIO;
}

int
nvrtx_vk_gpu_wait(void *owner, const struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	(void)ticket;
	return 0;
}

bool
nvrtx_vk_gpu_passed(void *owner, const struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	(void)ticket;
	return true;
}

void
nvrtx_vk_gpu_retire(void *owner, struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	ticket->fence = 0U;
}

void
nvrtx_vk_gpu_last(void *owner, struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	ticket->fence = 0U;
	ticket->submitted_us = 0U;
}

int
nvrtx_vk_gpu_idle(void *owner)
{
	(void)owner;
	return 0;
}

bool
nvrtx_vk_gpu_state_kept(void *owner)
{
	(void)owner;
	return false;
}

uint64_t
nvrtx_vk_gpu_quiet(void *owner, void *backing)
{
	(void)owner;
	(void)backing;
	return 0U;
}

void
nvrtx_vk_gpu_copied(void *owner, uint64_t bytes)
{
	(void)owner;
	(void)bytes;
}

void
nvrtx_vk_gpu_drawn(void *owner, uint32_t draws, uint32_t quads)
{
	(void)owner;
	(void)draws;
	(void)quads;
}

void
nvrtx_vk_blob_read(void *owner, uint8_t *to, const uint8_t *from, size_t length)
{
	(void)owner;
	memcpy(to, from, length);
}

/* Reports that the node decodes no video here. */
bool
nvrtx_vk_video_available(
	void *owner)
{
	(void)owner;

	/* Reports no decoder. */
	return false;
}

/* Refuses a decode, which the executor under test never reaches without a decoder. */
int
nvrtx_vk_video_decode(
	void *owner,
	uint64_t va,
	uint32_t bytes)
{
	(void)owner;
	(void)va;
	(void)bytes;

	/* Refuses the decode. */
	return EIO;
}

int
main(void)
{
	test_objects();
	test_stream();
	test_device();
	test_push();
	printf("%s nvrtx vk: %u checks, %u failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
	return failures == 0 ? 0 : 1;
}

static void
check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static int
destroy_payload(struct nvrtx_vk_session *session, void *payload)
{
	(void)session;
	destroyed++;
	free(payload);
	return 0;
}

/* Publishes, finds, holds, removes and closes objects; payloads die with their last holder. */
static void
test_objects(void)
{
	struct nvrtx_vk_session *session;
	struct nvrtx_vk_object *object;
	struct nvrtx_vk_object *held;
	unsigned before;
	uint64_t id;

	before = allocations;
	check(nvrtx_vk_session_open(NULL, &session) == 0 && session != NULL, "objects: session opened");

	destroyed = 0;
	check(nvrtx_vk_object_publish(session, I915_VK_OBJ_BUFFER, 7, malloc(4), destroy_payload, &object) == 0, "objects: buffer 7 published");
	check(nvrtx_vk_object_find(session, I915_VK_OBJ_BUFFER, 7) == object, "objects: buffer 7 found");
	check(nvrtx_vk_object_find(session, I915_VK_OBJ_IMAGE, 7) == NULL, "objects: the kind is matched");
	check(nvrtx_vk_object_publish(session, I915_VK_OBJ_BUFFER, 7, (void *)1, destroy_payload, &held) == EEXIST && held == NULL, "objects: an identity is published once");
	check(nvrtx_vk_object_publish(session, I915_VK_OBJ_IMAGE, 7, malloc(4), destroy_payload, &held) == 0, "objects: the same identity of another kind");
	check(nvrtx_vk_object_publish(session, I915_VK_OBJ_BUFFER, 0, (void *)1, destroy_payload, &held) == EINVAL, "objects: the null identity refused");
	check(session->object_count == 2 && session->live_objects == 2, "objects: two published");

	/* A holder keeps the payload past the removal. */
	check(nvrtx_vk_object_release(object) == EBUSY, "objects: the registry's reference is not released by hand");
	check(nvrtx_vk_object_retain(object) == 0, "objects: retained");
	check(nvrtx_vk_object_remove(session, I915_VK_OBJ_BUFFER, 7) == 0 && destroyed == 0, "objects: removed, still held");
	check(nvrtx_vk_object_find(session, I915_VK_OBJ_BUFFER, 7) == NULL && session->object_count == 1 && session->live_objects == 2, "objects: no longer found, still live");
	check(nvrtx_vk_object_release(object) == 0 && destroyed == 1 && session->live_objects == 1, "objects: the last holder destroys");
	check(nvrtx_vk_object_remove(session, I915_VK_OBJ_BUFFER, 7) == ENOENT, "objects: a second removal finds nothing");

	/* Many identities over the buckets, then the close. */
	for (id = 100; id < 3100; id++)
		(void)nvrtx_vk_object_publish(session, I915_VK_OBJ_SAMPLER, id, malloc(4), destroy_payload, &held);
	check(session->object_count == 3001 && nvrtx_vk_object_find(session, I915_VK_OBJ_SAMPLER, 2345) != NULL, "objects: 3000 more, each found");
	check(nvrtx_vk_object_remove(session, I915_VK_OBJ_SAMPLER, 1000) == 0 && nvrtx_vk_object_find(session, I915_VK_OBJ_SAMPLER, 1000) == NULL && nvrtx_vk_object_find(session, I915_VK_OBJ_SAMPLER, 1001) != NULL, "objects: one removed from the middle");
	check(nvrtx_vk_session_close(&session) == 0 && session == NULL && destroyed == 3002, "objects: the close destroys every payload");
	check(allocations == before, "objects: everything allocated is freed");
}

/* The transport with libvulkan's writer: reply selection, the probe, blob streams and refusals. */
static void
test_stream(void)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	struct vulkan_writer external;
	unsigned before;
	int error;

	check(nvrtx_vk_session_open(NULL, &session) == 0, "stream: session opened");
	vulkan_writer_init(&client);
	vulkan_writer_init(&external);
	before = allocations;

	/* A selector, one body and the probe at a fixed place. */
	memset(reply_words, 0xa5, sizeof(reply_words));
	encode_selector(&client, 64);
	encode_body(&client, 0x12345678U);
	encode_trailer(&client, 44);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == 0 && dispatched == 1, "stream: body run");
	check(reply_words[4] == TEST_BODY_OPCODE && reply_words[5] == 0 && reply_words[6] == 0x12345678U, "stream: body's reply after the echoed opcode");
	check(reply_words[15] == GPU_OP_ENUMERATE_INSTANCE_VERSION && reply_words[16] == 0 && reply_words[17] == 1 && reply_words[18] == 0, "stream: probe's opcode, result and present version");
	check(reply_words[19] == VK_MAKE_VERSION(1, 1, 0), "stream: probe's version last");
	check(reply_words[3] == 0xa5a5a5a5U && reply_words[20] == 0xa5a5a5a5U, "stream: nothing outside the range written");

	/* A stream in a blob is copied before the first body changes the original. */
	encode_body(&external, 0x11111111U);
	encode_body(&external, 0x22222222U);
	memcpy(external_words, external.data, external.bytes);
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_external(&client, external.bytes, 0);
	encode_trailer(&client, 44);
	mutate_external = 1;
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == 0 && dispatched == 3 && allocations == before, "stream: both bodies of the blob stream run, the copy freed");
	check(external_words[5] == 0xbadbadbaU && reply_words[6] == 0x11111111U && reply_words[9] == 0x22222222U, "stream: the copy, not the changed original, ran");
	mutate_external = 0;

	/* A blob stream with flags is refused before anything runs or completes. */
	memset(reply_words, 0, sizeof(reply_words));
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_external(&client, external.bytes, 1);
	encode_trailer(&client, 44);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == ENOTSUP && dispatched == 3 && reply_words[19] == 0 && allocations == before, "stream: flags refused, no probe answered");
	check(session->failed_opcode == GPU_OP_EXECUTE_STREAMS, "stream: the refused command remembered");

	/* A stream that names itself stops at the depth bound and frees every copy. */
	external.bytes = 0;
	encode_external(&external, 64, 0);
	memcpy(external_words, external.data, external.bytes);
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_external(&client, external.bytes, 0);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == E2BIG && allocations == before, "stream: nesting bounded, copies freed");

	/* The selected capacity bounds the replies. */
	client.bytes = 0;
	encode_selector(&client, 20);
	encode_body(&client, 0);
	encode_body(&client, 1);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == EINVAL, "stream: a reply past the capacity refused");
	client.bytes = 0;
	encode_selector(&client, 4096);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == EINVAL, "stream: a range past the blob refused");

	/* No selection, a misaligned probe, a cut header. */
	client.bytes = 0;
	encode_trailer(&client, 44);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == EINVAL, "stream: a probe without a reply stream refused");
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_trailer(&client, 43);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, dispatch_body);
	check(error == EINVAL, "stream: a misaligned seek refused");
	error = nvrtx_vk_stream_execute(session, client.data, 3, dispatch_body);
	check(error == EINVAL, "stream: a cut header refused");

	/* An unknown opcode with the real dispatcher (no handler owns it yet). */
	client.bytes = 0;
	encode_selector(&client, 64);
	encode_body(&client, 0);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, nvrtx_vk_dispatch);
	check(error == ENOTSUP, "stream: an opcode no handler owns refused");

	vulkan_writer_finish(&external);
	vulkan_writer_finish(&client);
	check(nvrtx_vk_session_close(&session) == 0, "stream: session closed");
}

static void
encode_selector(struct vulkan_writer *writer, uint64_t capacity)
{
	vulkan_write_u32(writer, GPU_OP_SET_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 16);
	vulkan_write_u64(writer, capacity);
}

static void
encode_trailer(struct vulkan_writer *writer, uint64_t offset)
{
	vulkan_write_u32(writer, GPU_OP_SEEK_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, offset);
	vulkan_write_u32(writer, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);
}

static void
encode_external(struct vulkan_writer *writer, uint64_t bytes, uint32_t flags)
{
	vulkan_write_u32(writer, GPU_OP_EXECUTE_STREAMS);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, 2);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, bytes);
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, flags);
}

static void
encode_body(struct vulkan_writer *writer, uint32_t bits)
{
	vulkan_write_u32(writer, TEST_BODY_OPCODE);
	vulkan_write_u32(writer, 1);
	vulkan_write_u32(writer, bits);
}

static int
dispatch_body(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply)
{
	uint32_t bits;

	(void)session;
	if (opcode != TEST_BODY_OPCODE || requested != 1)
		return EINVAL;
	bits = drv_i915_wire_read_u32(reader);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, bits);
	dispatched++;
	if (mutate_external != 0)
		external_words[5] = 0xbadbadbaU;
	return 0;
}

/* Reads the next word of the reply array. */
static uint32_t
reply_word(unsigned *at)
{
	uint32_t word;

	word = reply_words[*at];
	(*at)++;
	return word;
}

/*
 * The instance and device commands as libvulkan writes them, through the
 * real dispatcher: the instance, the one physical device, its memory and
 * queue family, a format, the device and its queue.
 */
static void
test_device(void)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	unsigned at;
	int error;

	check(nvrtx_vk_session_open(NULL, &session) == 0, "device: session opened");
	vulkan_writer_init(&client);

	/* vkCreateInstance with no create info (absent), identity 0x100. */
	memset(reply_words, 0, sizeof(reply_words));
	encode_selector(&client, 200);
	vulkan_write_u32(&client, GPU_OP_CREATE_INSTANCE);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x100);
	/* vkEnumeratePhysicalDevices with one id 0x101. */
	vulkan_write_u32(&client, GPU_OP_ENUMERATE_PHYSICAL_DEVICES);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x100);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x101);
	/* vkGetPhysicalDeviceQueueFamilyProperties with room for one. */
	vulkan_write_u32(&client, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x101);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, nvrtx_vk_dispatch);
	check(error == 0, "device: instance, enumeration, families run");
	at = 4;
	check(reply_word(&at) == GPU_OP_CREATE_INSTANCE && reply_word(&at) == 0 && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 0x100, "device: instance reply");
	check(reply_word(&at) == 0, "device: instance's high id word");
	check(reply_word(&at) == GPU_OP_ENUMERATE_PHYSICAL_DEVICES && reply_word(&at) == 0 && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 1, "device: one physical device");
	check(reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 0x101, "device: its identity echoed");
	(void)reply_word(&at);
	check(reply_word(&at) == GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 1, "device: families' present word and count");
	check(reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT) && reply_word(&at) == 1, "device: one family of one graphics queue");
	check(nvrtx_vk_object_find(session, I915_VK_OBJ_INSTANCE, 0x100) != NULL && nvrtx_vk_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, 0x101) != NULL, "device: instance and physical device remembered");

	/* vkGetPhysicalDeviceFormatProperties for B8G8R8A8_UNORM, then for D32 (none). */
	client.bytes = 0;
	memset(reply_words, 0, sizeof(reply_words));
	encode_selector(&client, 200);
	vulkan_write_u32(&client, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x101);
	vulkan_write_u32(&client, VK_FORMAT_B8G8R8A8_UNORM);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x101);
	vulkan_write_u32(&client, VK_FORMAT_D32_SFLOAT);
	vulkan_write_u64(&client, 1);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, nvrtx_vk_dispatch);
	check(error == 0, "device: format properties run");
	at = 4;
	check(reply_word(&at) == GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES && reply_word(&at) == 1 && reply_word(&at) == 0, "device: format reply's present word");
	check((reply_word(&at) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 && (reply_word(&at) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 && reply_word(&at) == 0, "device: BGRA renders and samples in both tilings, no buffer feature");
	check(reply_word(&at) == GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 0 && reply_word(&at) == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT && reply_word(&at) == 0, "device: depth is a depth attachment of optimal tiling only");

	/* vkGetPhysicalDeviceFormatProperties for D24_UNORM_S8_UINT: a depth and stencil attachment too. */
	client.bytes = 0;
	memset(reply_words, 0, sizeof(reply_words));
	encode_selector(&client, 200);
	vulkan_write_u32(&client, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x101);
	vulkan_write_u32(&client, VK_FORMAT_D24_UNORM_S8_UINT);
	vulkan_write_u64(&client, 1);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, nvrtx_vk_dispatch);
	at = 4;
	check(error == 0 && reply_word(&at) == GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES && reply_word(&at) == 1 && reply_word(&at) == 0 &&
	      reply_word(&at) == 0 && reply_word(&at) == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT && reply_word(&at) == 0,
	      "device: D24_UNORM_S8_UINT is a depth and stencil attachment of optimal tiling only");

	/* vkGetPhysicalDeviceMemoryProperties: one coherent device-local type. */
	client.bytes = 0;
	memset(reply_words, 0, sizeof(reply_words));
	encode_selector(&client, 2000);
	vulkan_write_u32(&client, GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x101);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 32);
	vulkan_write_u64(&client, 16);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, nvrtx_vk_dispatch);
	check(error == 0, "device: memory properties run");
	at = 4;
	check(reply_word(&at) == GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 1, "device: one memory type");
	check(reply_word(&at) == 32 && reply_word(&at) == 0, "device: the types' array of 32");
	check(reply_word(&at) == (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && reply_word(&at) == 0, "device: device-local, host-visible, coherent, heap 0");

	/* vkCreateDevice (no create info), its queue 0x103, vkDeviceWaitIdle, then the destroys. */
	client.bytes = 0;
	memset(reply_words, 0, sizeof(reply_words));
	encode_selector(&client, 200);
	vulkan_write_u32(&client, GPU_OP_CREATE_DEVICE);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x101);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x102);
	vulkan_write_u32(&client, GPU_OP_GET_DEVICE_QUEUE2);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x102);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 3);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x103);
	vulkan_write_u32(&client, GPU_OP_DEVICE_WAIT_IDLE);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 0x102);
	vulkan_write_u32(&client, GPU_OP_DESTROY_DEVICE);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0x102);
	vulkan_write_u64(&client, 0);
	error = nvrtx_vk_stream_execute(session, client.data, (uint32_t)client.bytes, nvrtx_vk_dispatch);
	check(error == 0, "device: device, queue, wait and destroy run");
	at = 4;
	check(reply_word(&at) == GPU_OP_CREATE_DEVICE && reply_word(&at) == 0 && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 0x102, "device: device reply");
	(void)reply_word(&at);
	check(reply_word(&at) == GPU_OP_GET_DEVICE_QUEUE2 && reply_word(&at) == 1 && reply_word(&at) == 0 && reply_word(&at) == 0x103, "device: queue reply");
	(void)reply_word(&at);
	check(reply_word(&at) == GPU_OP_DEVICE_WAIT_IDLE && reply_word(&at) == 0, "device: idle");
	check(nvrtx_vk_object_find(session, I915_VK_OBJ_DEVICE, 0x102) == NULL && nvrtx_vk_object_find(session, I915_VK_OBJ_QUEUE, 0x103) != NULL, "device: device forgotten, queue kept");
	check(session->timelines == (1ULL << 3), "device: the queue's timeline recorded");

	vulkan_writer_finish(&client);
	check(nvrtx_vk_session_close(&session) == 0, "device: session closed");
}

/*
 * Finds the last word a push buffer writes to a method (its headers
 * incrementing, as the executor writes them); 0 when no word does.
 */
static int
push_value(const uint8_t *push, size_t bytes, uint32_t method, uint32_t *value)
{
	uint32_t header;
	uint32_t count;
	uint32_t first;
	uint32_t index;
	size_t at;
	int found;

	found = 0;
	at = 0;
	while (at + 4 <= bytes) {
		memcpy(&header, push + at, 4);
		count = (header >> 16) & 0x1fffU;
		first = (header & 0x1fffU) << 2;
		at += 4;
		for (index = 0; index < count && at + 4 <= bytes; index++, at += 4) {
			if (first + index * 4U == method) {
				memcpy(value, push + at, 4);
				found = 1;
			}
		}
	}
	return found;
}

/* The executor's push buffer: the depth target, an indexed draw, the index buffer, the culling, the depth test and a uniform buffer. */
static void
test_push(void)
{
	static uint8_t push[4096];
	struct nvrtx_exec_target target;
	struct nvrtx_exec_depth depth;
	uint32_t value;
	size_t at;

	/* A colour target with a depth target beside it: ZF32 in one-GOB blocks, its width the row's pixels. */
	memset(&target, 0, sizeof(target));
	target.address = 0x200000000ULL;
	target.width = 1920;
	target.height = 1080;
	target.pitch = 7680;
	target.format = VK_FORMAT_B8G8R8A8_UNORM;
	target.depth_address = 0x300000200ULL;
	target.depth_pitch = 7680;
	target.depth_height = 1080;
	at = nvrtx_exec_target(push, 0, &target);
	check(push_value(push, at, 0x0fe0, &value) && value == 3 && push_value(push, at, 0x0fe4, &value) && value == 0x200, "push: the depth target's address");
	check(push_value(push, at, 0x0fe8, &value) && value == 0x0a && push_value(push, at, 0x0fec, &value) && value == 0, "push: ZF32 in one-GOB blocks");
	check(push_value(push, at, 0x0ff0, &value) && value == 7680U * 1080U / 4U, "push: the depth target's array pitch");
	check(push_value(push, at, 0x1538, &value) && value == 1, "push: one depth target selected");
	check(push_value(push, at, 0x1228, &value) && value == 1920 && push_value(push, at, 0x122c, &value) && value == 1080 &&
	      push_value(push, at, 0x1230, &value) && value == 1, "push: the depth target's size");
	target.depth_stencil = 1;
	at = nvrtx_exec_target(push, 0, &target);
	check(push_value(push, at, 0x0fe8, &value) && value == 0x16, "push: S8Z24 for a depth target with a stencil");
	target.depth_stencil = 0;
	target.depth_address = 0;
	at = nvrtx_exec_target(push, 0, &target);
	check(push_value(push, at, 0x1538, &value) && value == 0 && !push_value(push, at, 0x0fe0, &value), "push: no depth target without one");

	/* The stencil test, two-sided: the front face adds where it passes, the back face takes away; off without it. */
	memset(&depth, 0, sizeof(depth));
	depth.stencil = 1;
	depth.front.fail = VK_STENCIL_OP_KEEP;
	depth.front.depth_fail = VK_STENCIL_OP_ZERO;
	depth.front.pass = VK_STENCIL_OP_INCREMENT_AND_WRAP;
	depth.front.compare = VK_COMPARE_OP_ALWAYS;
	depth.front.compare_mask = 0x0f;
	depth.front.write_mask = 0xff;
	depth.front.reference = 3;
	depth.back = depth.front;
	depth.back.pass = VK_STENCIL_OP_DECREMENT_AND_WRAP;
	depth.back.compare = VK_COMPARE_OP_NOT_EQUAL;
	depth.back.write_mask = 0x7f;
	at = nvrtx_exec_depth(push, 0, &depth);
	check(push_value(push, at, 0x1380, &value) && value == 1 && push_value(push, at, 0x1594, &value) && value == 1, "push: the stencil test on, two-sided");
	check(push_value(push, at, 0x1384, &value) && value == 1 && push_value(push, at, 0x1388, &value) && value == 2 &&
	      push_value(push, at, 0x138c, &value) && value == 7 && push_value(push, at, 0x1390, &value) && value == 0x207,
	      "push: the front face's operations (D3D values) and function (OGL)");
	check(push_value(push, at, 0x1394, &value) && value == 3 && push_value(push, at, 0x1398, &value) && value == 0x0f &&
	      push_value(push, at, 0x139c, &value) && value == 0xff, "push: the front face's reference, compare mask and write mask");
	check(push_value(push, at, 0x15a0, &value) && value == 8 && push_value(push, at, 0x15a4, &value) && value == 0x205 &&
	      push_value(push, at, 0x0f54, &value) && value == 3 && push_value(push, at, 0x0f58, &value) && value == 0x7f &&
	      push_value(push, at, 0x0f5c, &value) && value == 0x0f, "push: the back face's operation, function, reference and masks");
	depth.stencil = 0;
	at = nvrtx_exec_depth(push, 0, &depth);
	check(push_value(push, at, 0x1380, &value) && value == 0 && !push_value(push, at, 0x1384, &value), "push: the stencil test off");

	/* A clear of the stencil only: its value, the rectangle, the stencil bit. */
	at = nvrtx_exec_clear_stencil(push, 0, 0x1ab, 0, 0, 64, 32);
	check(push_value(push, at, 0x0da0, &value) && value == 0xab && push_value(push, at, 0x19d0, &value) && value == 2,
	      "push: a stencil clear of the low 8 bits, the stencil only");

	/* A block-linear colour target: its width the row's pixels, one-GOB blocks, its array pitch the level. */
	target.block_linear = 1;
	target.width = 1000;
	target.height = 100;
	target.pitch = 4032;
	at = nvrtx_exec_target(push, 0, &target);
	check(push_value(push, at, 0x0808, &value) && value == 1008 && push_value(push, at, 0x0814, &value) && value == 0 &&
	      push_value(push, at, 0x081c, &value) && value == 4032U * 104U / 4U, "push: a block-linear colour target");

	/* A tiled texture's header: block linear, 2D, the image's last level and the view's levels. */
	{
		static const uint32_t identity[4] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
		uint32_t header[8];

		nvrtx_exec_texture_header_tiled(header, 0x512345600ULL, 1024, 512, 11, 2, 9, VK_FORMAT_R8G8B8A8_UNORM, identity);
		check(((header[2] >> 21) & 7U) == 3U && ((header[4] >> 23) & 15U) == 1U, "push: a tiled header is block linear and 2D");
		check((header[1] >> 9) == (0x12345600U >> 9) && (header[2] & 0xffffU) == 5U && ((header[1] >> 6) & 7U) == 0U, "push: a tiled header's address");
		check((header[3] >> 28) == 10U && (header[7] & 15U) == 2U && ((header[7] >> 4) & 15U) == 10U, "push: a tiled header's levels");
		check((header[4] & 0xffffU) == 1023U && (header[5] & 0xffffU) == 511U, "push: a tiled header's size");
	}

	/* An indexed draw: the vertex offset as the bases, the instances, the first index and the count. */
	at = nvrtx_exec_draw_indexed(push, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 6, 36, 5000, 2, -3);
	check(push_value(push, at, 0x1618, &value) && value == (4U | (1U << 31)), "push: an indexed draw begins triangles, the instances iterated");
	check(push_value(push, at, 0x17dc, &value) && value == 6 && push_value(push, at, 0x17e0, &value) && value == 36, "push: the first index and the count");
	check(push_value(push, at, 0x0220, &value) && value == 5000 && push_value(push, at, 0x1438, &value) && value == 2, "push: the instances from the first");
	check(push_value(push, at, 0x1434, &value) && value == 0xfffffffdU && push_value(push, at, 0x1118, &value) && value == 0xfffffffdU, "push: the vertex offset");
	check(push_value(push, at, 0x1644, &value) && value == 0, "push: no primitive restart");

	/* The index buffer: its address, its bytes and 16-bit indices. */
	at = nvrtx_exec_index_buffer(push, 0, 0x123456700ULL, 4096, VK_INDEX_TYPE_UINT16);
	check(push_value(push, at, 0x17c8, &value) && value == 1 && push_value(push, at, 0x17cc, &value) && value == 0x23456700U, "push: the index buffer's address");
	check(push_value(push, at, 0x023c, &value) && value == 4096 && push_value(push, at, 0x17d8, &value) && value == 1, "push: the index buffer's size and 16-bit indices");
	at = nvrtx_exec_index_buffer(push, 0, 0x1000, 4096, VK_INDEX_TYPE_UINT32);
	check(push_value(push, at, 0x17d8, &value) && value == 2, "push: 32-bit indices");

	/* The culling and the front face. */
	at = nvrtx_exec_raster(push, 0, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	check(push_value(push, at, 0x1918, &value) && value == 1 && push_value(push, at, 0x1920, &value) && value == 0x405 &&
	      push_value(push, at, 0x191c, &value) && value == 0x901, "push: back faces culled, counterclockwise in front");
	at = nvrtx_exec_raster(push, 0, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	check(push_value(push, at, 0x1918, &value) && value == 0 && !push_value(push, at, 0x1920, &value) &&
	      push_value(push, at, 0x191c, &value) && value == 0x900, "push: nothing culled, clockwise in front");

	/* The depth test, and a clear of depth. */
	depth.test = 1;
	depth.write = 1;
	depth.compare = VK_COMPARE_OP_LESS_OR_EQUAL;
	at = nvrtx_exec_depth(push, 0, &depth);
	check(push_value(push, at, 0x12cc, &value) && value == 1 && push_value(push, at, 0x12e8, &value) && value == 1 &&
	      push_value(push, at, 0x130c, &value) && value == 0x203, "push: the depth test, its writes and LEQUAL");
	depth.test = 0;
	at = nvrtx_exec_depth(push, 0, &depth);
	check(push_value(push, at, 0x12cc, &value) && value == 0 && push_value(push, at, 0x12e8, &value) && value == 0, "push: no depth test, no writes");
	at = nvrtx_exec_clear_depth(push, 0, 0x3f800000U, 0, 0, 1920, 1080);
	check(push_value(push, at, 0x0d90, &value) && value == 0x3f800000U && push_value(push, at, 0x19d0, &value) && value == 1, "push: a clear of depth only to 1.0");

	/* A uniform buffer at slot 1 of the fragment program's group. */
	at = nvrtx_exec_uniform(push, 0, 4, 1, 0x400000100ULL, 256);
	check(push_value(push, at, 0x2380, &value) && value == 256 && push_value(push, at, 0x2384, &value) && value == 4 &&
	      push_value(push, at, 0x2388, &value) && value == 0x100, "push: the uniform buffer's size and address");
	check(push_value(push, at, 0x2410 + 4 * 0x20, &value) && value == 0x11, "push: bound at slot 1 of group 4");
}
