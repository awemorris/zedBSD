/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for the Vulkan executor's entry, router, transport, wire
 * codec, object table, instance commands and fence commands
 * (src/drivers/gpu/i915/render: vulkan.c, dispatch.c, transport.c, codec.c,
 * object.c, instance.c, fence.c).
 *
 * The graphics path (gfx.h) is replaced by stand-ins that claim one chosen
 * opcode each, so the routing around it can be checked.  A session blob is
 * a host buffer whose "physical address" is its host pointer.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel services the executor calls, backed by the host. */
static unsigned fixture_live;
static char fixture_log[256];

void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);
void kern_io_write_barrier(void);
void kern_logf(const char *format, ...);

void *
kern_malloc(size_t size)
{
	void *pointer = malloc(size);
	if (pointer != NULL)
		fixture_live++;
	return pointer;
}

void *
kern_calloc(size_t count, size_t size)
{
	void *pointer = calloc(count, size);
	if (pointer != NULL)
		fixture_live++;
	return pointer;
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		fixture_live--;
	free(pointer);
}

void kern_io_write_barrier(void) { }

void
kern_logf(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(fixture_log, sizeof(fixture_log), format, arguments);
	va_end(arguments);
}

#include "../../../src/drivers/gpu/i915/render/codec.c"
#include "../../../src/drivers/gpu/i915/render/object.c"
#include "../../../src/drivers/gpu/i915/render/dispatch.c"
#include "../../../src/drivers/gpu/i915/render/transport.c"
#include "../../../src/drivers/gpu/i915/render/instance.c"
#include "../../../src/drivers/gpu/i915/render/vulkan.c"
#include "../../../src/drivers/gpu/i915/render/fence.c"

/* A session blob's physical address is its host pointer in this fixture. */
void *
kern_pmem_to_kernel(hal_physaddr_t address)
{
	return (void *)(uintptr_t)address;
}

/* The graphics path stand-ins: each claims one opcode and records that it did. */
static uint32_t gfx_obj_opcode = UINT32_MAX;
static uint32_t gfx_rec_opcode = UINT32_MAX;
static int gfx_claimed;
static unsigned gfx_closed;

int
drv_i915_gfx_obj_dispatch(struct i915_render_session *s, uint32_t op, struct i915_wire_reader *r, struct i915_wire_writer *w, int *handled)
{
	(void)s; (void)r; (void)w;
	*handled = 0;
	if (op != gfx_obj_opcode)
		return 0;
	*handled = 1;
	gfx_claimed = 1;
	return 0;
}

int
drv_i915_gfx_rec_dispatch(struct i915_render_session *s, uint32_t op, struct i915_wire_reader *r, struct i915_wire_writer *w, int *handled)
{
	(void)s; (void)r; (void)w;
	*handled = 0;
	if (op != gfx_rec_opcode)
		return 0;
	*handled = 1;
	gfx_claimed = 2;
	return EIO;
}

void
drv_i915_gfx_session_close(struct i915_render_session *session)
{
	(void)session;
	gfx_closed++;
}

void
drv_i915_gfx_objects_release(struct i915_render_session *session)
{
	(void)session;
}

/* A little-endian stream builder that mirrors the libvulkan wire writer. */
struct builder {
	uint8_t bytes[512];
	size_t size;
};

static void
put32(struct builder *b, uint32_t value)
{
	b->bytes[b->size++] = (uint8_t)value;
	b->bytes[b->size++] = (uint8_t)(value >> 8);
	b->bytes[b->size++] = (uint8_t)(value >> 16);
	b->bytes[b->size++] = (uint8_t)(value >> 24);
}

static void
put64(struct builder *b, uint64_t value)
{
	put32(b, (uint32_t)value);
	put32(b, (uint32_t)(value >> 32));
}

static uint32_t
get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t
get64(const uint8_t *bytes)
{
	return (uint64_t)get32(bytes) | (uint64_t)get32(bytes + 4) << 32;
}

/* The fixture's executor, node session and executor session. */
static struct i915_device fixture_device;
static struct i915_session fixture_gpu;
static struct i915_render_device *fixture_vk;
static struct i915_render_session *fixture_session;

/* Two session blobs: the reply blob (slot 3) and an external stream blob (slot 5). */
static uint8_t reply_blob[4096];
static uint8_t stream_blob[64];
static struct i915_gem_object reply_object;
static struct i915_gem_object stream_object;

static void
fixture_open(void)
{
	int error;

	memset(&fixture_device, 0, sizeof(fixture_device));
	fixture_device.product = 0x46a6U;
	memset(&fixture_gpu, 0, sizeof(fixture_gpu));

	memset(&reply_object, 0, sizeof(reply_object));
	reply_object.slot = 3U;
	reply_object.bytes = sizeof(reply_blob);
	reply_object.run.paddr = (hal_physaddr_t)(uintptr_t)reply_blob;

	memset(&stream_object, 0, sizeof(stream_object));
	stream_object.slot = 5U;
	stream_object.bytes = sizeof(stream_blob);
	stream_object.run.paddr = (hal_physaddr_t)(uintptr_t)stream_blob;

	reply_object.session_next = &stream_object;
	fixture_gpu.objects = &reply_object;

	error = drv_i915_render_attach(&fixture_device, &fixture_vk);
	assert(error == 0);
	error = drv_i915_render_open(fixture_vk, &fixture_gpu, &fixture_session);
	assert(error == 0);
}

static void
fixture_close(void)
{
	drv_i915_render_close(fixture_session);
	drv_i915_render_detach(fixture_vk);
}

/* Opens a stream with the 36-byte reply selector naming slot 3 at `offset`. */
static void
put_selector(struct builder *b, uint32_t slot, uint64_t offset)
{
	put32(b, 178U);
	put32(b, 0U);
	put64(b, 1U);
	put32(b, slot);
	put64(b, offset);
	put64(b, sizeof(reply_blob) - offset);
}

/* Runs a stream the way the node's command operation does. */
static int
run_stream(const struct builder *b, size_t *reply_bytes, uint8_t **reply)
{
	size_t capacity;
	void *region;
	int error;

	region = drv_i915_render_transport_reply(&fixture_gpu, b->bytes, (uint32_t)b->size, &capacity);
	error = drv_i915_render_execute(fixture_session, b->bytes, b->size, region, region != NULL ? &capacity : NULL);
	*reply_bytes = capacity;
	*reply = region;
	return error;
}

static void
test_object_table(void)
{
	struct i915_render_device vk;
	struct i915_render_session one;
	struct i915_render_session two;
	int marker[300];
	int other;
	unsigned index;
	int error;

	memset(&vk, 0, sizeof(vk));
	memset(&one, 0, sizeof(one));
	memset(&two, 0, sizeof(two));
	one.vk = &vk;
	two.vk = &vk;
	error = drv_i915_object_table_create(&vk.objects);
	assert(error == 0);

	/* Insert grows past the initial capacity and every identity resolves. */
	for (index = 0U; index < 300U; index++) {
		error = drv_i915_object_insert(&one, I915_VK_OBJ_BUFFER, index + 1U, &marker[index]);
		assert(error == 0);
	}
	for (index = 0U; index < 300U; index++)
		assert(drv_i915_object_lookup(&one, I915_VK_OBJ_BUFFER, index + 1U) == &marker[index]);

	/* An identity of another kind does not collide. */
	assert(drv_i915_object_lookup(&one, I915_VK_OBJ_IMAGE, 1U) == NULL);

	/* The same identity in another session is another object. */
	assert(drv_i915_object_lookup(&two, I915_VK_OBJ_BUFFER, 1U) == NULL);
	error = drv_i915_object_insert(&two, I915_VK_OBJ_BUFFER, 1U, &other);
	assert(error == 0);
	assert(drv_i915_object_lookup(&two, I915_VK_OBJ_BUFFER, 1U) == &other);
	assert(drv_i915_object_lookup(&one, I915_VK_OBJ_BUFFER, 1U) == &marker[0]);

	/* Reinserting an identity replaces the object in place. */
	error = drv_i915_object_insert(&one, I915_VK_OBJ_BUFFER, 1U, &marker[7]);
	assert(error == 0);
	assert(drv_i915_object_lookup(&one, I915_VK_OBJ_BUFFER, 1U) == &marker[7]);
	assert(drv_i915_object_lookup(&two, I915_VK_OBJ_BUFFER, 1U) == &other);

	/* Remove drops exactly one identity and keeps the rest resolvable. */
	drv_i915_object_remove(&one, I915_VK_OBJ_BUFFER, 1U);
	assert(drv_i915_object_lookup(&one, I915_VK_OBJ_BUFFER, 1U) == NULL);
	assert(drv_i915_object_lookup(&one, I915_VK_OBJ_BUFFER, 2U) == &marker[1]);
	assert(drv_i915_object_lookup(&two, I915_VK_OBJ_BUFFER, 1U) == &other);

	/* Forgetting a session drops all its identities and none of another's. */
	drv_i915_object_forget(&one);
	for (index = 1U; index < 300U; index++)
		assert(drv_i915_object_lookup(&one, I915_VK_OBJ_BUFFER, index + 1U) == NULL);
	assert(drv_i915_object_lookup(&two, I915_VK_OBJ_BUFFER, 1U) == &other);

	drv_i915_object_table_destroy(vk.objects);
}

static void
test_codec(void)
{
	uint8_t buffer[64];
	uint8_t reply[32];
	uint8_t scratch[64];
	struct i915_wire_reader reader;
	struct i915_wire_writer writer;
	struct i915_wire_arena arena;
	struct builder b;
	const char *text;
	uint32_t word;
	uint64_t wide;
	void *array;

	/* A writer lays down little-endian words a reader recovers exactly. */
	writer.base = buffer; writer.size = sizeof(buffer); writer.offset = 0U; writer.error = 0;
	drv_i915_wire_reply_u32(&writer, 0x01020304U);
	drv_i915_wire_reply_u64(&writer, 0x1122334455667788ULL);
	assert(writer.error == 0);
	assert(writer.offset == 12U);
	assert(buffer[0] == 0x04 && buffer[3] == 0x01);

	reader.base = buffer; reader.size = writer.offset; reader.offset = 0U; reader.error = 0;
	word = drv_i915_wire_read_u32(&reader);
	wide = drv_i915_wire_read_handle(&reader);
	assert(reader.error == 0);
	assert(word == 0x01020304U);
	assert(wide == 0x1122334455667788ULL);

	/* Reading past the end latches the error and stops advancing. */
	word = drv_i915_wire_read_u32(&reader);
	assert(reader.error != 0);
	assert(word == 0U);
	assert(drv_i915_wire_read_array(&reader, 1U, 1U) == NULL);

	/* A reply that overflows its buffer latches the writer error. */
	writer.base = reply; writer.size = 6U; writer.offset = 0U; writer.error = 0;
	drv_i915_wire_reply_u64(&writer, 0U);
	assert(writer.error != 0);

	/* Padded bytes: three bytes and one byte of zero padding. */
	writer.base = reply; writer.size = sizeof(reply); writer.offset = 0U; writer.error = 0;
	memset(reply, 0xee, sizeof(reply));
	i915_vkc_reply_bytes(&writer, "abc", 3U);
	assert(writer.offset == 4U && reply[3] == 0U);

	/* A string: its length includes the terminator; a missing terminator is supplied. */
	b.size = 0U;
	put64(&b, 3U);
	memcpy(b.bytes + b.size, "xyz\0", 4U);
	b.size += 4U;
	arena.base = scratch; arena.size = sizeof(scratch); arena.used = 0U;
	reader.base = b.bytes; reader.size = b.size; reader.offset = 0U; reader.error = 0;
	text = i915_vkc_read_string(&reader, &arena);
	assert(text != NULL && strcmp(text, "xy") == 0);
	assert(reader.offset == 12U);

	/* An element count beyond the bytes that remain never sizes an allocation. */
	reader.base = b.bytes; reader.size = 4U; reader.offset = 0U; reader.error = 0;
	array = i915_vkc_array(&reader, &arena, 5U, 1U);
	assert(array == NULL && reader.error != 0);

	/* A request larger than the arena fails. */
	reader.base = b.bytes; reader.size = b.size; reader.offset = 0U; reader.error = 0;
	arena.used = 60U;
	array = i915_vkc_array(&reader, &arena, 8U, 1U);
	assert(array == NULL && reader.error != 0);

	/* The optional external chain: absent reads one word, present reads four. */
	b.size = 0U;
	put64(&b, 1U); put32(&b, 1000072002U); put64(&b, 0U); put32(&b, 1U);
	reader.base = b.bytes; reader.size = b.size; reader.offset = 0U; reader.error = 0;
	i915_vkc_skip_external_chain(&reader);
	assert(reader.error == 0 && reader.offset == b.size);
}

static void
test_routing(void)
{
	static const uint32_t unported[6] = {21U, 23U, 61U, 84U, 18U, 106U};
	static const char *const modules[6] = {"res", "res", "pipe", "pipe", "cmdbuf", "cmdbuf"};
	static const uint32_t refused[2] = {17U, 148U};
	struct builder b;
	uint8_t *reply;
	size_t reply_bytes;
	unsigned index;
	int error;

	fixture_open();

	/* The graphics objects and the recording are asked first and own what they claim. */
	gfx_obj_opcode = 21U;
	gfx_claimed = 0;
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 21U); put32(&b, 0U);
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == 0 && gfx_claimed == 1);

	gfx_rec_opcode = 106U;
	gfx_claimed = 0;
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 106U); put32(&b, 0U);
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == EIO && gfx_claimed == 2);
	gfx_obj_opcode = UINT32_MAX;
	gfx_rec_opcode = UINT32_MAX;

	/*
	 * An opcode the graphics path does not own and whose range names an
	 * unported module is refused: the stream stops, the version probe after
	 * it does not run, and no reply length is published.
	 */
	for (index = 0U; index < 6U; index++) {
		b.size = 0U;
		put_selector(&b, 3U, 0U);
		put32(&b, unported[index]); put32(&b, 1U);
		put32(&b, 137U); put32(&b, 1U); put64(&b, 1U);
		memset(reply_blob, 0xee, sizeof(reply_blob));
		fixture_log[0] = '\0';
		error = run_stream(&b, &reply_bytes, &reply);
		assert(error == ENOTSUP);
		assert(reply_bytes == sizeof(reply_blob));
		assert(reply_blob[4] == 0xeeU);
		assert(strstr(fixture_log, "i915: XXX ") == fixture_log);
		assert(strstr(fixture_log, modules[index]) != NULL);
	}

	/*
	 * The sync range belongs to the fence commands: a sync opcode other than
	 * the four fence commands is refused by fence.c the same way.
	 */
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 42U); put32(&b, 1U);
	put32(&b, 137U); put32(&b, 1U); put64(&b, 1U);
	memset(reply_blob, 0xee, sizeof(reply_blob));
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == ENOTSUP);
	assert(reply_bytes == sizeof(reply_blob));
	assert(reply_blob[4] == 0xeeU);
	assert(strcmp(fixture_log, "i915: vk: XXX unimplemented opcode 42 (sync)\n") == 0);

	/* An unimplemented builtin is refused the same way. */
	for (index = 0U; index < 2U; index++) {
		b.size = 0U;
		put_selector(&b, 3U, 0U);
		put32(&b, refused[index]); put32(&b, 1U);
		put32(&b, 137U); put32(&b, 1U); put64(&b, 1U);
		memset(reply_blob, 0xee, sizeof(reply_blob));
		error = run_stream(&b, &reply_bytes, &reply);
		assert(error == ENOTSUP);
		assert(reply_bytes == sizeof(reply_blob));
		assert(reply_blob[4] == 0xeeU);
		assert(strcmp(fixture_log, "i915: vk: XXX unimplemented opcode 17 (builtin)\n") == 0 || refused[index] != 17U);
	}

	fixture_close();
}

static void
test_transport(void)
{
	struct builder b;
	struct builder nested;
	uint8_t *reply;
	size_t reply_bytes;
	size_t capacity;
	int error;

	fixture_open();

	/* The selector names slot 3 at offset 16; the region runs to the end of the blob. */
	b.size = 0U;
	put_selector(&b, 3U, 16U);
	assert(drv_i915_render_transport_reply(&fixture_gpu, b.bytes, (uint32_t)b.size, &capacity) == reply_blob + 16);
	assert(capacity == sizeof(reply_blob) - 16U);

	/* A short stream, another first opcode, an unknown slot or an offset past the end select nothing. */
	assert(drv_i915_render_transport_reply(&fixture_gpu, b.bytes, 35U, &capacity) == NULL && capacity == 0U);
	b.bytes[0] = 177U;
	assert(drv_i915_render_transport_reply(&fixture_gpu, b.bytes, (uint32_t)b.size, &capacity) == NULL);
	b.size = 0U;
	put_selector(&b, 9U, 0U);
	assert(drv_i915_render_transport_reply(&fixture_gpu, b.bytes, (uint32_t)b.size, &capacity) == NULL);
	b.size = 0U;
	put_selector(&b, 3U, sizeof(reply_blob));
	assert(drv_i915_render_transport_reply(&fixture_gpu, b.bytes, (uint32_t)b.size, &capacity) == NULL);

	/* select -> seek 64 -> version probe: the 20-byte trailer at its fixed position. */
	b.size = 0U;
	put_selector(&b, 3U, 16U);
	put32(&b, 179U); put32(&b, 0U); put64(&b, 64U);
	put32(&b, 137U); put32(&b, 1U); put64(&b, 1U);
	memset(reply_blob, 0, sizeof(reply_blob));
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == 0);
	assert(reply_bytes == 84U);
	assert(get32(reply_blob + 16 + 64) == 137U);
	assert(get32(reply_blob + 16 + 68) == 0U);
	assert(get64(reply_blob + 16 + 72) == 1U);
	assert(get32(reply_blob + 16 + 80) == ((1U << 22) | (1U << 12)));

	/* A seek past the region is refused. */
	b.size = 0U;
	put_selector(&b, 3U, 16U);
	put32(&b, 179U); put32(&b, 0U); put64(&b, 5000U);
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == EINVAL);

	/* A stream that selects nothing but asks for a reply overflows an empty region. */
	b.size = 0U;
	put32(&b, 137U); put32(&b, 1U); put64(&b, 1U);
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == EMSGSIZE && reply == NULL);

	/* 180: the external stream in slot 5 is decoded into the same reply. */
	nested.size = 0U;
	put32(&nested, 137U); put32(&nested, 1U); put64(&nested, 1U);
	memcpy(stream_blob + 8, nested.bytes, nested.size);
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 180U); put32(&b, 0U);
	put32(&b, 1U); put64(&b, 1U); put32(&b, 5U); put64(&b, 8U); put64(&b, nested.size);
	put64(&b, 0U); put32(&b, 0U); put64(&b, 0U); put32(&b, 0U);
	memset(reply_blob, 0, sizeof(reply_blob));
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == 0);
	assert(reply_bytes == 20U);
	assert(get32(reply_blob) == 137U);
	assert(get32(reply_blob + 16) == ((1U << 22) | (1U << 12)));

	/* 180 bounds: a range past the blob, an empty stream and an unknown slot are refused. */
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 180U); put32(&b, 0U);
	put32(&b, 1U); put64(&b, 1U); put32(&b, 5U); put64(&b, 60U); put64(&b, 8U);
	put64(&b, 0U); put32(&b, 0U); put64(&b, 0U); put32(&b, 0U);
	assert(run_stream(&b, &reply_bytes, &reply) == EINVAL);
	b.bytes[36 + 8 + 4 + 8 + 4 + 8] = 0U;	/* offset 0, bytes 0 */
	memset(b.bytes + 36 + 8 + 4 + 8 + 4, 0, 16);
	assert(run_stream(&b, &reply_bytes, &reply) == EINVAL);
	b.bytes[36 + 8 + 4 + 8] = 7U;		/* slot 7 */
	assert(run_stream(&b, &reply_bytes, &reply) == EINVAL);

	fixture_close();
}

static void
test_instance(void)
{
	struct builder b;
	uint8_t *reply;
	size_t reply_bytes;
	int error;

	fixture_open();

	/* vkCreateInstance without create info, vkEnumeratePhysicalDevices, vkCreateDevice-less queue. */
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 0U); put32(&b, 1U); put64(&b, 0U); put64(&b, 0U); put64(&b, 1U); put64(&b, 0x1234U);
	put32(&b, 2U); put32(&b, 1U); put64(&b, 0x1234U); put64(&b, 1U); put32(&b, 1U); put64(&b, 1U); put64(&b, 0x5678U);
	put32(&b, 6U); put32(&b, 1U); put64(&b, 0x5678U); put64(&b, 1U);
	put32(&b, 20U); put32(&b, 1U); put64(&b, 0x9abcU);
	memset(reply_blob, 0, sizeof(reply_blob));
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == 0);

	/* [0][VK_SUCCESS][present][identity] */
	assert(get32(reply_blob) == 0U && get32(reply_blob + 4) == 0U);
	assert(get64(reply_blob + 8) == 1U && get64(reply_blob + 16) == 0x1234U);
	assert(drv_i915_object_lookup(fixture_session, I915_VK_OBJ_INSTANCE, 0x1234U) == &i915_instance_token);

	/* [2][VK_SUCCESS][present][count 1][array 1][identity] */
	assert(get32(reply_blob + 24) == 2U && get32(reply_blob + 28) == 0U);
	assert(get32(reply_blob + 40) == 1U && get64(reply_blob + 44) == 1U && get64(reply_blob + 52) == 0x5678U);
	assert(drv_i915_object_lookup(fixture_session, I915_VK_OBJ_PHYSICAL_DEVICE, 0x5678U) != NULL);

	/* [6][present][apiVersion][driverVersion][vendorID][deviceID]... */
	assert(get32(reply_blob + 60) == 6U && get64(reply_blob + 64) == 1U);
	assert(get32(reply_blob + 72) == ((1U << 22) | (1U << 12)));
	assert(get32(reply_blob + 80) == 0x8086U && get32(reply_blob + 84) == 0x46a6U);

	/* The wait-idle reply is its echoed opcode and VK_SUCCESS, last in the reply. */
	assert(get32(reply_blob + reply_bytes - 8U) == 20U && get32(reply_blob + reply_bytes - 4U) == 0U);

	/* vkDestroyInstance forgets the identity. */
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 1U); put32(&b, 0U); put64(&b, 0x1234U); put64(&b, 0U);
	error = run_stream(&b, &reply_bytes, &reply);
	assert(error == 0 && reply_bytes == 0U);
	assert(drv_i915_object_lookup(fixture_session, I915_VK_OBJ_INSTANCE, 0x1234U) == NULL);

	/* Two physical devices are malformed. */
	b.size = 0U;
	put_selector(&b, 3U, 0U);
	put32(&b, 2U); put32(&b, 1U); put64(&b, 0x1234U); put64(&b, 1U); put32(&b, 2U); put64(&b, 2U);
	assert(run_stream(&b, &reply_bytes, &reply) == EINVAL);

	fixture_close();
}

static void
test_lifetime(void)
{
	struct gpu_capset capset;
	unsigned before;
	unsigned closed;
	int error;

	before = fixture_live;
	closed = gfx_closed;

	/* Attach builds the executor and the 168-byte capset with vendor flags 7. */
	fixture_log[0] = '\0';
	fixture_open();
	assert(strstr(fixture_log, "vendor flags 7") != NULL);
	assert(fixture_vk->capset_bytes == 168U);
	assert(fixture_vk->capset[0] == 1U && fixture_vk->capset[1] == 0x0040310DU);
	assert(fixture_vk->capset[38] == 1U && fixture_vk->capset[40] == 0x5a424453U && fixture_vk->capset[41] == 7U);
	assert(fixture_session->vk == fixture_vk && fixture_session->gpu == &fixture_gpu);

	/* The capset must fit the requested capacity. */
	memset(&capset, 0, sizeof(capset));
	capset.capacity = 100U;
	assert(drv_i915_render_get_capset(fixture_vk, &capset) == EINVAL);
	capset.capacity = sizeof(capset.data);
	error = drv_i915_render_get_capset(fixture_vk, &capset);
	assert(error == 0 && capset.bytes == 168U && memcmp(capset.data, fixture_vk->capset, 168U) == 0);

	/* Close releases the graphics session state; nothing leaks. */
	fixture_close();
	assert(gfx_closed == closed + 1U);
	assert(fixture_live == before);
	assert(drv_i915_render_errno(0) == 0 && drv_i915_render_errno(-2) == ENOMEM && drv_i915_render_errno(-3) == EINVAL);
}

int
main(void)
{
	test_object_table();
	test_codec();
	test_routing();
	test_transport();
	test_instance();
	test_lifetime();
	printf("i915 vk cmd host test PASS\n");
	return 0;
}
