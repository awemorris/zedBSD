/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises descriptor API ownership against independent native byte parsing. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "internal.h"

#define TEST_OBJECTS 512U
#define TEST_BINDINGS 8U
#define TEST_BATCH 40U
#define TEST_ALLOCATIONS 512U
#define TEST_DEVICE_ID 1000U

/* A byte cursor checks exact field widths without using the library's decoder. */
struct peer_reader {
	const uint8_t *bytes;
	size_t size;
	size_t cursor;
};

/* Each peer binding records native declaration metadata independently of local layouts. */
struct peer_binding {
	uint32_t number;
	uint32_t type;
	uint32_t count;
	uint32_t stages;
	uint32_t immutable_count;
	uint64_t samplers[2];
};

/* Native identities and set-pool relationships survive independently of local pointers. */
struct peer_object {
	uint32_t kind;
	uint64_t pool;
	uint32_t count;
	struct peer_binding bindings[TEST_BINDINGS];
};

/* One observed descriptor element retains the fields the real native peer receives. */
struct peer_descriptor {
	uint32_t binding;
	uint32_t element;
	uint32_t type;
	uint64_t sampler;
	uint64_t resource;
	uint64_t offset;
	uint64_t range;
	uint32_t layout;
};

/* A fixture transaction owns native state and the last complete update payload. */
struct peer_state {
	struct peer_object objects[TEST_OBJECTS];
	struct peer_descriptor descriptors[16];
	uint32_t descriptor_count;
	uint32_t copies;
	uint32_t calls[80];
	uint32_t fail_opcode;
	VkResult fail_status;
	uint64_t copied_source;
	uint64_t copied_destination;
};

/* Compatible callbacks share allocation mechanics while exposing distinct userdata. */
struct callback_state {
	unsigned live;
	unsigned object_frees;
	unsigned command_frees;
	unsigned fail_object;
	unsigned fail_command;
};

/* Each callback allocation remains tracked until the actual matching free operation. */
struct callback_allocation {
	void *address;
	size_t bytes;
	VkSystemAllocationScope scope;
	struct callback_state *owner;
};

/* The serial fixture owns native state for its entire process lifetime. */
static struct peer_state peer;

/* Callback records allow compatible destruction userdata without losing creator accounting. */
static struct callback_allocation callback_allocations[TEST_ALLOCATIONS];

static uint32_t peer_u32(struct peer_reader *reader);
static uint64_t peer_u64(struct peer_reader *reader);
static void expect_u32(struct peer_reader *reader, uint32_t expected);
static void expect_u64(struct peer_reader *reader, uint64_t expected);
static void expect_structure(struct peer_reader *reader, VkStructureType structure);
static uint64_t peer_layout(struct peer_reader *reader, VkResult status);
static uint64_t peer_pool(struct peer_reader *reader, VkResult status);
static void peer_allocate(struct peer_reader *reader, struct vulkan_writer *response, VkResult status);
static void peer_free_sets(struct peer_reader *reader, VkResult status);
static void peer_update(struct peer_reader *reader);
static void peer_clear_pool(uint64_t pool);
static struct vulkan_object *fake_object(struct VkDevice_T *device, enum vulkan_object_kind kind);
static unsigned child_count(const struct vulkan_object *parent);
static unsigned native_set_count(uint64_t pool);
static uint64_t wire_id(uint64_t handle);
static void *callback_allocate(void *userdata, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void *callback_reallocate(void *userdata, void *original, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void callback_free(void *userdata, void *address);
static void callbacks_init(VkAllocationCallbacks *callbacks, struct callback_state *state);
static struct callback_allocation *callback_find(void *address);
static void test_batches(struct VkDevice_T *device);
static void test_updates(struct VkDevice_T *device, void *guard);
static void test_destroy_loss(void);
static VkDescriptorPool create_pool(struct VkDevice_T *device, const VkAllocationCallbacks *allocator);
static VkDescriptorSetLayout create_layout(struct VkDevice_T *device, const VkDescriptorSetLayoutBinding *bindings, uint32_t count, const VkAllocationCallbacks *allocator);
static VkResult allocate_sets(struct VkDevice_T *device, VkDescriptorPool pool, VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *sets);

/*
 * Runs the eight descriptor APIs with native and local ownership observed separately.
 */
int
main(void)
{
	struct vulkan_context context;
	struct VkDevice_T device;
	void *guard;
	unsigned index;
	int error;

	/* The surrounding device is a stable identity; the APIs own all tested children. */
	memset(&context, 0, sizeof(context));
	memset(&device, 0, sizeof(device));
	device.object.context = &context;
	device.object.wire_id = TEST_DEVICE_ID;

	/* An inaccessible page makes accidental reads of ignored input pointers observable. */
	guard = mmap(NULL, 4096U, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(guard != MAP_FAILED);
	test_batches(&device);
	test_updates(&device, guard);
	test_destroy_loss();
	error = munmap(guard, 4096U);
	assert(error == 0);

	/* Both namespaces and every callback allocation must be empty after normal teardown. */
	assert(device.object.first_child == NULL);
	assert(context.error == VK_SUCCESS);
	for (index = 0U; index < TEST_OBJECTS; index++)
		assert(peer.objects[index].kind == 0U);

	/* No command or object may retain callback storage after its owning call completes. */
	for (index = 0U; index < TEST_ALLOCATIONS; index++)
		assert(callback_allocations[index].address == NULL);

	/* Every selected API must have reached the independent native peer. */
	for (index = 72U; index <= 79U; index++)
		assert(peer.calls[index] != 0U);

	/* Succeeded: batch rollback, update framing and ignored-field semantics all held. */
	puts("libvulkan descriptors: PASS (40 sets, rollback, detached layouts, guarded inputs, callbacks)");
	return 0;
}

/*
 * Parses production requests independently and returns native result-bearing replies.
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
	uint32_t opcode;
	uint64_t identity;
	uint64_t pool;
	VkResult status;

	/* A lost namespace cannot accept another transaction. */
	status = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* The peer does not use any library decoder to read a native request. */
	request.bytes = writer->data;
	request.size = writer->bytes;
	request.cursor = 0U;
	opcode = peer_u32(&request);
	assert(opcode >= 72U);
	assert(opcode <= 79U);
	expect_u32(&request, 1U);
	expect_u64(&request, TEST_DEVICE_ID);
	peer.calls[opcode]++;
	status = VK_SUCCESS;

	/* An injected native rejection leaves remote object ownership unchanged. */
	if (peer.fail_opcode == opcode) {
		status = peer.fail_status;
		peer.fail_opcode = 0U;
	}

	/* Replies are independent allocations which the real command decoder will consume. */
	vulkan_writer_init(&response);
	vulkan_write_u32(&response, opcode);

	/* Each operation changes the peer only at its specified native success boundary. */
	switch (opcode) {
	case 72U:
		/* Layout creation preserves declared binding order and immutable sampler IDs. */
		identity = peer_layout(&request, status);
		vulkan_write_u32(&response, (uint32_t)status);
		vulkan_write_u64(&response, 1U);
		vulkan_write_u64(&response, identity);
		break;
	case 74U:
		/* Pool creation starts an empty native child namespace. */
		identity = peer_pool(&request, status);
		vulkan_write_u32(&response, (uint32_t)status);
		vulkan_write_u64(&response, 1U);
		vulkan_write_u64(&response, identity);
		break;
	case 73U:
	case 75U:
		/* Destroying a layout never destroys its previously allocated native sets. */
		identity = peer_u64(&request);
		expect_u64(&request, 0U);
		assert(identity < TEST_OBJECTS);
		object = &peer.objects[identity];
		assert(object->kind != 0U);

		/* A destroyed pool consumes every native set child in the same operation. */
		if (opcode == 75U)
			peer_clear_pool(identity);

		/* This native object no longer exists after successful void destruction. */
		memset(object, 0, sizeof(*object));
		break;
	case 76U:
		/* Native reset failures must preserve the complete set namespace. */
		pool = peer_u64(&request);
		expect_u32(&request, 0U);
		assert(pool < TEST_OBJECTS);
		assert(peer.objects[pool].kind == 74U);

		/* Success atomically invalidates every set allocated from this pool. */
		if (status == VK_SUCCESS)
			peer_clear_pool(pool);

		/* Preserve the exact native VkResult for the real public wrapper. */
		vulkan_write_u32(&response, (uint32_t)status);
		break;
	case 77U:
		/* Sets are published remotely only if the complete native allocation succeeds. */
		peer_allocate(&request, &response, status);
		break;
	case 78U:
		/* Failed native free leaves every requested descriptor identity alive. */
		peer_free_sets(&request, status);
		vulkan_write_u32(&response, (uint32_t)status);
		break;
	case 79U:
		/* Record only wire-visible fields; no application descriptor pointer is consulted. */
		peer_update(&request);
		break;
	default:
		/* The focused API family cannot send unrelated Vulkan commands. */
		abort();
	}

	/* The independently parsed request must contain neither omitted nor extra fields. */
	assert(request.cursor == request.size);
	assert(response.error == VK_SUCCESS);
	assert(response.bytes <= reply_capacity);
	vulkan_reader_init(reply, response.data, response.bytes);

	/* Succeeded: only the reply carries native failure; the transport itself completed. */
	return VK_SUCCESS;
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

/* Parses a native layout declaration and publishes its peer metadata on success. */
static uint64_t
peer_layout(
	struct peer_reader *reader,
	VkResult status)
{
	struct peer_object parsed;
	struct peer_binding *binding;
	uint32_t index;
	uint32_t sampler;
	uint64_t identity;
	uint64_t count;

	/* The native create-info pointer is mandatory and contains an ordinary core record. */
	memset(&parsed, 0, sizeof(parsed));
	expect_u64(reader, 1U);
	expect_structure(reader, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
	expect_u32(reader, 0U);
	parsed.count = peer_u32(reader);
	assert(parsed.count <= TEST_BINDINGS);
	expect_u64(reader, parsed.count);

	/* Binding declaration order and immutable-array cardinality are preserved exactly. */
	for (index = 0U; index < parsed.count; index++) {
		binding = &parsed.bindings[index];
		binding->number = peer_u32(reader);
		binding->type = peer_u32(reader);
		binding->count = peer_u32(reader);
		binding->stages = peer_u32(reader);
		count = peer_u64(reader);
		assert(count <= 2U);
		binding->immutable_count = (uint32_t)count;

		/* A nonempty immutable array must contain exactly the declared descriptor count. */
		if (count != 0U)
			assert(count == binding->count);

		/* Only selected sampler bindings may reference immutable sampler identities. */
		for (sampler = 0U; sampler < binding->immutable_count; sampler++) {
			identity = peer_u64(reader);
			assert(identity < TEST_OBJECTS);
			assert(peer.objects[identity].kind == 100U + VULKAN_OBJECT_SAMPLER);
			binding->samplers[sampler] = identity;
		}
	}

	/* Application allocation callbacks stay local; the native output receives a fresh ID. */
	expect_u64(reader, 0U);
	expect_u64(reader, 1U);
	identity = peer_u64(reader);
	assert(identity < TEST_OBJECTS);
	assert(peer.objects[identity].kind == 0U);

	/* Native failure owns no corresponding layout. */
	if (status == VK_SUCCESS) {
		parsed.kind = 72U;
		peer.objects[identity] = parsed;
	}

	/* Succeeded: the reply echoes the caller's reserved identity. */
	return identity;
}

/* Parses a descriptor pool's complete type-capacity array and native output marker. */
static uint64_t
peer_pool(
	struct peer_reader *reader,
	VkResult status)
{
	uint64_t identity;
	uint32_t index;

	/* The fixture requests every descriptor kind it uses and ordinary per-set free. */
	expect_u64(reader, 1U);
	expect_structure(reader, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	expect_u32(reader, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
	expect_u32(reader, 64U);
	expect_u32(reader, 8U);
	expect_u64(reader, 8U);

	/* Every pool-size record is two 32-bit fields with no implicit C padding. */
	for (index = 0U; index < 8U; index++) {
		expect_u32(reader, index);
		expect_u32(reader, 256U + index);
	}

	/* Renderer allocators remain absent from all public callback policies. */
	expect_u64(reader, 0U);
	expect_u64(reader, 1U);
	identity = peer_u64(reader);
	assert(identity < TEST_OBJECTS);
	assert(peer.objects[identity].kind == 0U);

	/* Only native success creates a pool namespace. */
	if (status == VK_SUCCESS)
		peer.objects[identity].kind = 74U;

	/* Succeeded: this ID is returned by the exact native creation reply. */
	return identity;
}

/* Allocates native sets atomically while preserving the requested output-array shape. */
static void
peer_allocate(
	struct peer_reader *reader,
	struct vulkan_writer *response,
	VkResult status)
{
	uint64_t layouts[64];
	uint64_t sets[64];
	uint64_t pool;
	uint32_t count;
	uint32_t index;

	/* A valid native allocation describes its pool before two distinct handle arrays. */
	expect_u64(reader, 1U);
	expect_structure(reader, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	pool = peer_u64(reader);
	assert(pool < TEST_OBJECTS);
	assert(peer.objects[pool].kind == 74U);
	count = peer_u32(reader);
	assert(count <= 64U);
	expect_u64(reader, count);

	/* Layout handles must be live native objects at allocation time. */
	for (index = 0U; index < count; index++) {
		layouts[index] = peer_u64(reader);
		assert(layouts[index] < TEST_OBJECTS);
		assert(peer.objects[layouts[index]].kind == 72U);
	}

	/* The output array contains reserved IDs, not already-created native sets. */
	expect_u64(reader, count);
	for (index = 0U; index < count; index++) {
		sets[index] = peer_u64(reader);
		assert(sets[index] < TEST_OBJECTS);
		assert(peer.objects[sets[index]].kind == 0U);
	}

	/* Native allocation failure still returns the same array length with null outputs. */
	vulkan_write_u32(response, (uint32_t)status);
	vulkan_write_u64(response, count);
	for (index = 0U; index < count; index++) {
		if (status == VK_SUCCESS) {
			peer.objects[sets[index]] = peer.objects[layouts[index]];
			peer.objects[sets[index]].kind = 77U;
			peer.objects[sets[index]].pool = pool;
			vulkan_write_u64(response, sets[index]);
		} else {
			vulkan_write_u64(response, 0U);
		}
	}

	/* Succeeded: the peer owns either every requested set or none of them. */
	return;
}

/* Parses a complete free batch before changing any native set ownership. */
static void
peer_free_sets(
	struct peer_reader *reader,
	VkResult status)
{
	uint64_t sets[64];
	uint64_t pool;
	uint32_t count;
	uint32_t index;

	/* Free accepts only sets which still belong to the named live pool. */
	pool = peer_u64(reader);
	assert(pool < TEST_OBJECTS);
	assert(peer.objects[pool].kind == 74U);
	count = peer_u32(reader);
	assert(count <= 64U);
	expect_u64(reader, count);
	for (index = 0U; index < count; index++) {
		sets[index] = peer_u64(reader);
		assert(sets[index] < TEST_OBJECTS);
		assert(peer.objects[sets[index]].kind == 77U);
		assert(peer.objects[sets[index]].pool == pool);
	}

	/* Failed native free preserves the entire batch. */
	if (status != VK_SUCCESS)
		return;

	/* Successful free consumes each selected native identity exactly once. */
	for (index = 0U; index < count; index++)
		memset(&peer.objects[sets[index]], 0, sizeof(peer.objects[sets[index]]));

	/* Succeeded: these native sets no longer exist. */
	return;
}

/* Records writes and copies with exact native framing and without reading application input. */
static void
peer_update(
	struct peer_reader *reader)
{
	struct peer_descriptor *descriptor;
	uint64_t set;
	uint64_t count;
	uint32_t writes;
	uint32_t index;
	uint32_t element;
	uint32_t binding;
	uint32_t first;
	uint32_t descriptors;
	uint32_t type;
	uint32_t copies;

	/* The native call describes all writes before any descriptor copy. */
	peer.descriptor_count = 0U;
	peer.copies = 0U;
	writes = peer_u32(reader);
	expect_u64(reader, writes);
	for (index = 0U; index < writes; index++) {
		expect_structure(reader, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
		set = peer_u64(reader);
		assert(set < TEST_OBJECTS);
		assert(peer.objects[set].kind == 77U);
		binding = peer_u32(reader);
		first = peer_u32(reader);
		descriptors = peer_u32(reader);
		type = peer_u32(reader);

		/* Each image-info element carries sampler, view and layout in that fixed order. */
		count = peer_u64(reader);
		for (element = 0U; element < count; element++) {
			assert(peer.descriptor_count < 16U);
			descriptor = &peer.descriptors[peer.descriptor_count];
			memset(descriptor, 0, sizeof(*descriptor));
			descriptor->binding = binding;
			descriptor->element = first + element;
			descriptor->type = type;
			descriptor->sampler = peer_u64(reader);
			descriptor->resource = peer_u64(reader);
			descriptor->layout = peer_u32(reader);
			peer.descriptor_count++;
		}

		/* Only image descriptor types may carry this native array. */
		if (type <= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
			assert(count == descriptors);
		} else {
			assert(count == 0U);
		}

		/* Buffer descriptors retain 64-bit offsets and ranges, including WHOLE_SIZE. */
		count = peer_u64(reader);
		for (element = 0U; element < count; element++) {
			assert(peer.descriptor_count < 16U);
			descriptor = &peer.descriptors[peer.descriptor_count];
			memset(descriptor, 0, sizeof(*descriptor));
			descriptor->binding = binding;
			descriptor->element = first + element;
			descriptor->type = type;
			descriptor->resource = peer_u64(reader);
			descriptor->offset = peer_u64(reader);
			descriptor->range = peer_u64(reader);
			peer.descriptor_count++;
		}

		/* The fixture's buffer writes use the ordinary uniform-buffer descriptor type. */
		if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
			assert(count == descriptors);
		} else {
			assert(count == 0U);
		}

		/* Texel-buffer views form a separate native handle array. */
		count = peer_u64(reader);
		for (element = 0U; element < count; element++) {
			assert(peer.descriptor_count < 16U);
			descriptor = &peer.descriptors[peer.descriptor_count];
			memset(descriptor, 0, sizeof(*descriptor));
			descriptor->binding = binding;
			descriptor->element = first + element;
			descriptor->type = type;
			descriptor->resource = peer_u64(reader);
			peer.descriptor_count++;
		}

		/* No other descriptor type may smuggle an inactive texel-view pointer. */
		if (type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
			assert(count == descriptors);
		} else {
			assert(count == 0U);
		}
	}

	/* Copies preserve source and destination binding coordinates after every write. */
	copies = peer_u32(reader);
	expect_u64(reader, copies);
	for (index = 0U; index < copies; index++) {
		expect_structure(reader, VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
		peer.copied_source = peer_u64(reader);
		expect_u32(reader, 5U);
		expect_u32(reader, 0U);
		peer.copied_destination = peer_u64(reader);
		expect_u32(reader, 5U);
		expect_u32(reader, 0U);
		expect_u32(reader, 1U);
		peer.copies++;
	}

	/* Succeeded: the peer captured only independently decoded wire-visible fields. */
	return;
}

/* Invalidates only descriptor sets owned by one native pool. */
static void
peer_clear_pool(
	uint64_t pool)
{
	unsigned index;

	/* Layouts and unrelated pools retain their independent native lifetimes. */
	for (index = 0U; index < TEST_OBJECTS; index++) {
		if (peer.objects[index].pool == pool)
			memset(&peer.objects[index], 0, sizeof(peer.objects[index]));
	}

	/* Succeeded: the selected pool has no native set children. */
	return;
}

/* Creates only the surrounding sampler or resource object needed by descriptor inputs. */
static struct vulkan_object *
fake_object(
	struct VkDevice_T *device,
	enum vulkan_object_kind kind)
{
	struct vulkan_object *object;
	VkResult status;

	/* Descriptor API tests do not substitute any layout, pool or set implementation. */
	status = vulkan_object_alloc(sizeof(*object), sizeof(uint64_t), kind, &device->object, device->object.context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	assert(status == VK_SUCCESS);
	status = vulkan_object_reserve_id(object);
	assert(status == VK_SUCCESS);
	status = vulkan_object_publish(object);
	assert(status == VK_SUCCESS);
	assert(object->wire_id < TEST_OBJECTS);
	peer.objects[object->wire_id].kind = 100U + kind;

	/* Succeeded: the fixture owns one ordinary resource handle for descriptor encoding. */
	return object;
}

/* Counts real local child ownership without interpreting private descriptor subtype storage. */
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

	/* Succeeded: each remaining child owns one live local set allocation. */
	return count;
}

/* Counts native sets independently of the local parent list. */
static unsigned
native_set_count(
	uint64_t pool)
{
	unsigned index;
	unsigned count;

	/* Each native record belongs to at most one pool. */
	count = 0U;
	for (index = 0U; index < TEST_OBJECTS; index++) {
		if (peer.objects[index].pool == pool)
			count++;
	}

	/* Succeeded: reports only peer-owned native descriptor sets. */
	return count;
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

	/* Descriptor objects and their command temporaries must use the effective policy. */
	state = userdata;
	assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT || scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);

	/* Fail a later set preparation without affecting earlier callback ownership. */
	if (scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT && state->fail_object != 0U) {
		state->fail_object--;

		/* The selected object acquires no host storage. */
		if (state->fail_object == 0U)
			return NULL;
	}

	/* A failed destruction command must retain uncertain native state under terminal context ownership. */
	if (scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) {
		if (state->fail_command != 0U) {
			state->fail_command--;
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

	/* Succeeded: this policy can be supplied to any ordinary descriptor creation. */
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

/* Creates a native pool with explicit capacity records to exercise the shared codec. */
static VkDescriptorPool
create_pool(
	struct VkDevice_T *device,
	const VkAllocationCallbacks *allocator)
{
	VkDescriptorPoolCreateInfo info;
	VkDescriptorPoolSize sizes[8];
	VkDescriptorPool pool;
	VkResult status;
	unsigned index;

	/* Deliberately distinct counts expose swaps or truncation in array encoding. */
	memset(sizes, 0, sizeof(sizes));
	for (index = 0U; index < 8U; index++) {
		sizes[index].type = (VkDescriptorType)index;
		sizes[index].descriptorCount = 256U + index;
	}

	/* This pool permits per-set frees and batches larger than the old finite subset. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	info.maxSets = 64U;
	info.poolSizeCount = 8U;
	info.pPoolSizes = sizes;
	status = vkCreateDescriptorPool((VkDevice)device, &info, allocator, &pool);
	assert(status == VK_SUCCESS);

	/* Succeeded: the public API owns one complete native pool. */
	return pool;
}

/* Creates a standard layout without relying on any descriptor-private structure. */
static VkDescriptorSetLayout
create_layout(
	struct VkDevice_T *device,
	const VkDescriptorSetLayoutBinding *bindings,
	uint32_t count,
	const VkAllocationCallbacks *allocator)
{
	VkDescriptorSetLayoutCreateInfo info;
	VkDescriptorSetLayout layout;
	VkResult status;

	/* The public binding array is the only metadata the implementation may copy. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = count;
	info.pBindings = bindings;
	status = vkCreateDescriptorSetLayout((VkDevice)device, &info, allocator, &layout);
	assert(status == VK_SUCCESS);

	/* Succeeded: the application owns this ordinary source layout. */
	return layout;
}

/* Allocates one batch through the public API with a caller-owned layout handle array. */
static VkResult
allocate_sets(
	struct VkDevice_T *device,
	VkDescriptorPool pool,
	VkDescriptorSetLayout layout,
	uint32_t count,
	VkDescriptorSet *sets)
{
	VkDescriptorSetLayout layouts[64];
	VkDescriptorSetAllocateInfo info;
	VkResult status;
	uint32_t index;

	/* Every requested set has the same valid source layout in these batch cases. */
	assert(count <= 64U);
	for (index = 0U; index < count; index++)
		layouts[index] = layout;

	/* The API receives independent layout and output arrays with explicit cardinality. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = pool;
	info.descriptorSetCount = count;
	info.pSetLayouts = layouts;
	status = vkAllocateDescriptorSets((VkDevice)device, &info, sets);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the entire requested set batch belongs to its pool. */
	return VK_SUCCESS;
}

/* Tests large batches, native failures, partial local rollback and reset ownership. */
static void
test_batches(
	struct VkDevice_T *device)
{
	VkAllocationCallbacks create_callbacks;
	VkAllocationCallbacks destroy_callbacks;
	struct callback_state creator;
	struct callback_state destroyer;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayout layout;
	VkDescriptorPool pool;
	VkDescriptorSet sets[TEST_BATCH];
	struct vulkan_object *pool_object;
	unsigned count;
	unsigned index;
	unsigned allocations_before;
	unsigned calls_before;
	VkResult status;

	/* Distinct compatible userdata exposes whether final destruction uses this call's policy. */
	memset(&creator, 0, sizeof(creator));
	memset(&destroyer, 0, sizeof(destroyer));
	callbacks_init(&create_callbacks, &creator);
	callbacks_init(&destroy_callbacks, &destroyer);
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layout = create_layout(device, &binding, 1U, &create_callbacks);
	pool = create_pool(device, &create_callbacks);
	pool_object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)pool);

	/* Forty sets exceed a small fixed batch and must all become native and local children. */
	status = allocate_sets(device, pool, layout, TEST_BATCH, sets);
	assert(status == VK_SUCCESS);
	count = child_count(pool_object);
	assert(count == TEST_BATCH);
	count = native_set_count(pool_object->wire_id);
	assert(count == TEST_BATCH);
	for (index = 0U; index < TEST_BATCH; index++)
		assert(sets[index] != VK_NULL_HANDLE);

	/* Failed native free leaves every requested handle usable for a successful retry. */
	peer.fail_opcode = 78U;
	peer.fail_status = VK_ERROR_OUT_OF_HOST_MEMORY;
	status = vkFreeDescriptorSets((VkDevice)device, pool, TEST_BATCH, sets);
	assert(status == VK_ERROR_OUT_OF_HOST_MEMORY);
	count = child_count(pool_object);
	assert(count == TEST_BATCH);
	count = native_set_count(pool_object->wire_id);
	assert(count == TEST_BATCH);
	status = vkFreeDescriptorSets((VkDevice)device, pool, TEST_BATCH, sets);
	assert(status == VK_SUCCESS);
	assert(pool_object->first_child == NULL);
	count = native_set_count(pool_object->wire_id);
	assert(count == 0U);

	/* Native allocation failure must return all nulls and unlink every prepared local set. */
	peer.fail_opcode = 77U;
	peer.fail_status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	allocations_before = creator.live;
	status = allocate_sets(device, pool, layout, TEST_BATCH, sets);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(pool_object->first_child == NULL);
	assert(creator.live == allocations_before);
	for (index = 0U; index < TEST_BATCH; index++)
		assert(sets[index] == VK_NULL_HANDLE);

	/* A later local allocation failure must undo earlier children before any native enqueue. */
	creator.fail_object = 7U;
	calls_before = peer.calls[77U];
	status = allocate_sets(device, pool, layout, TEST_BATCH, sets);
	assert(status == VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(peer.calls[77U] == calls_before);
	assert(pool_object->first_child == NULL);
	assert(creator.live == allocations_before);
	for (index = 0U; index < TEST_BATCH; index++)
		assert(sets[index] == VK_NULL_HANDLE);

	/* Failed native reset leaves the complete previously allocated child batch alive. */
	status = allocate_sets(device, pool, layout, TEST_BATCH, sets);
	assert(status == VK_SUCCESS);
	peer.fail_opcode = 76U;
	peer.fail_status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	status = vkResetDescriptorPool((VkDevice)device, pool, 0U);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	count = child_count(pool_object);
	assert(count == TEST_BATCH);
	count = native_set_count(pool_object->wire_id);
	assert(count == TEST_BATCH);
	status = vkResetDescriptorPool((VkDevice)device, pool, 0U);
	assert(status == VK_SUCCESS);
	assert(pool_object->first_child == NULL);
	assert(creator.live == allocations_before);

	/* Pool destruction consumes its remaining sets and uses compatible final userdata. */
	status = allocate_sets(device, pool, layout, 3U, sets);
	assert(status == VK_SUCCESS);
	vkDestroyDescriptorPool((VkDevice)device, pool, &destroy_callbacks);
	assert(creator.live == 1U);
	assert(destroyer.object_frees == 4U);
	assert(destroyer.command_frees != 0U);
	calls_before = destroyer.command_frees;
	assert(destroyer.live == 0U);

	/* The source layout has its own allocation and also honors compatible destruction userdata. */
	vkDestroyDescriptorSetLayout((VkDevice)device, layout, &destroy_callbacks);
	assert(creator.live == 0U);
	assert(destroyer.object_frees == 5U);
	assert(destroyer.command_frees > calls_before);

	/* Succeeded: every failure retained old owners and every success retired the right batch. */
	return;
}

/* Tests immutable rollover, detached layouts and ignored descriptor pointers with guard pages. */
static void
test_updates(
	struct VkDevice_T *device,
	void *guard)
{
	struct vulkan_object *sampler;
	struct vulkan_object *view;
	struct vulkan_object *buffer;
	struct vulkan_object *texel;
	struct peer_object *native_layout;
	VkDescriptorSetLayoutBinding bindings[TEST_BINDINGS];
	VkDescriptorSetLayout layout;
	VkDescriptorPool pool;
	VkDescriptorSet sets[2];
	VkSampler immutable[1];
	VkDescriptorImageInfo images[3];
	VkDescriptorBufferInfo buffer_info;
	VkBufferView texel_view;
	VkWriteDescriptorSet writes[6];
	VkCopyDescriptorSet copy;
	uint64_t layout_id;
	uint64_t source_id;
	uint64_t destination_id;
	uint32_t numbers[TEST_BINDINGS] = { 9U, 4U, 8U, 0U, 7U, 1U, 5U, 6U };
	unsigned index;
	VkResult status;

	/* These ordinary resource handles supply independent native IDs to the descriptor encoder. */
	sampler = fake_object(device, VULKAN_OBJECT_SAMPLER);
	view = fake_object(device, VULKAN_OBJECT_IMAGE_VIEW);
	buffer = fake_object(device, VULKAN_OBJECT_BUFFER);
	texel = fake_object(device, VULKAN_OBJECT_BUFFER_VIEW);
	immutable[0] = (VkSampler)(uintptr_t)vulkan_nondispatchable_handle(sampler);
	memset(bindings, 0, sizeof(bindings));

	/* Deliberately unordered numeric bindings make rollover follow binding numbers, not array order. */
	for (index = 0U; index < TEST_BINDINGS; index++) {
		bindings[index].binding = numbers[index];
		bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		bindings[index].descriptorCount = 1U;
		bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings[index].pImmutableSamplers = immutable;
	}

	/* Unused immutable pointers on non-sampler bindings must never be dereferenced. */
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[1].pImmutableSamplers = guard;
	bindings[2].descriptorCount = 0U;
	bindings[2].pImmutableSamplers = guard;
	bindings[3].pImmutableSamplers = NULL;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[6].pImmutableSamplers = guard;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
	bindings[7].pImmutableSamplers = guard;
	layout = create_layout(device, bindings, TEST_BINDINGS, NULL);
	layout_id = wire_id((uint64_t)(uintptr_t)layout);
	native_layout = &peer.objects[layout_id];
	assert(native_layout->count == TEST_BINDINGS);
	assert(native_layout->bindings[0].number == 9U);
	assert(native_layout->bindings[1].immutable_count == 0U);
	assert(native_layout->bindings[2].immutable_count == 0U);
	assert(native_layout->bindings[4].samplers[0] == sampler->wire_id);
	pool = create_pool(device, NULL);
	status = allocate_sets(device, pool, layout, 2U, sets);
	assert(status == VK_SUCCESS);
	source_id = wire_id((uint64_t)(uintptr_t)sets[0]);
	destination_id = wire_id((uint64_t)(uintptr_t)sets[1]);

	/* Destroy and poison all source layout storage before updating its previously allocated sets. */
	vkDestroyDescriptorSetLayout((VkDevice)device, layout, NULL);
	memset(bindings, 0xa5, sizeof(bindings));
	assert(peer.objects[layout_id].kind == 0U);
	assert(peer.objects[source_id].count == TEST_BINDINGS);

	/* Ignored sampler and view handles must never be resolved as ordinary local object pointers. */
	memset(images, 0, sizeof(images));
	images[0].sampler = (VkSampler)(uintptr_t)guard;
	images[0].imageView = (VkImageView)(uintptr_t)vulkan_nondispatchable_handle(view);
	images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	images[1].sampler = (VkSampler)(uintptr_t)guard;
	images[1].imageView = images[0].imageView;
	images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	images[2].sampler = immutable[0];
	images[2].imageView = (VkImageView)(uintptr_t)guard;
	images[2].imageLayout = (VkImageLayout)UINT32_MAX;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.buffer = (VkBuffer)(uintptr_t)vulkan_nondispatchable_handle(buffer);
	buffer_info.offset = UINT64_C(0x100002030);
	buffer_info.range = VK_WHOLE_SIZE;
	texel_view = (VkBufferView)(uintptr_t)vulkan_nondispatchable_handle(texel);

	/* All inactive arrays begin inaccessible; each write selects only its actual representation. */
	memset(writes, 0, sizeof(writes));
	for (index = 0U; index < 6U; index++) {
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = sets[0];
		writes[index].descriptorCount = 1U;
		writes[index].pImageInfo = guard;
		writes[index].pBufferInfo = guard;
		writes[index].pTexelBufferView = guard;
	}

	/* One immutable sampler write rolls from binding7 past empty8 into immutable9. */
	writes[0].dstBinding = 7U;
	writes[0].descriptorCount = 2U;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[1].dstBinding = 1U;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &images[0];
	writes[2].dstBinding = 4U;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	writes[2].pImageInfo = &images[1];
	writes[3].dstBinding = 0U;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[3].pImageInfo = &images[2];
	writes[4].dstBinding = 5U;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[4].pBufferInfo = &buffer_info;
	writes[5].dstBinding = 6U;
	writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
	writes[5].pTexelBufferView = &texel_view;

	/* A copy after these writes retains both native set identities and binding coordinates. */
	memset(&copy, 0, sizeof(copy));
	copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
	copy.srcSet = sets[0];
	copy.srcBinding = 5U;
	copy.dstSet = sets[1];
	copy.dstBinding = 5U;
	copy.descriptorCount = 1U;
	vkUpdateDescriptorSets((VkDevice)device, 6U, writes, 1U, &copy);
	assert(device->object.context->error == VK_SUCCESS);
	assert(peer.descriptor_count == 7U);

	/* Both immutable sampler elements ignore the complete inaccessible image-info pointer. */
	assert(peer.descriptors[0].sampler == 0U);
	assert(peer.descriptors[0].resource == 0U);
	assert(peer.descriptors[0].layout == VK_IMAGE_LAYOUT_UNDEFINED);
	assert(peer.descriptors[1].sampler == 0U);
	assert(peer.descriptors[1].resource == 0U);
	assert(peer.descriptors[1].layout == VK_IMAGE_LAYOUT_UNDEFINED);

	/* Combined and sampled images ignore sampler fields but preserve the live image-view ID. */
	assert(peer.descriptors[2].sampler == 0U);
	assert(peer.descriptors[2].resource == view->wire_id);
	assert(peer.descriptors[2].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	assert(peer.descriptors[3].sampler == 0U);
	assert(peer.descriptors[3].resource == view->wire_id);
	assert(peer.descriptors[3].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	/* Mutable sampler writes preserve only the sampler; its ignored view and layout remain untouched. */
	assert(peer.descriptors[4].sampler == sampler->wire_id);
	assert(peer.descriptors[4].resource == 0U);
	assert(peer.descriptors[4].layout == VK_IMAGE_LAYOUT_UNDEFINED);

	/* Buffer and texel representations preserve 64-bit fields and omit both inactive arrays. */
	assert(peer.descriptors[5].resource == buffer->wire_id);
	assert(peer.descriptors[5].offset == buffer_info.offset);
	assert(peer.descriptors[5].range == VK_WHOLE_SIZE);
	assert(peer.descriptors[6].resource == texel->wire_id);
	assert(peer.copies == 1U);
	assert(peer.copied_source == source_id);
	assert(peer.copied_destination == destination_id);

	/* Set storage and copied binding metadata are consumed by ordinary pool teardown. */
	vkDestroyDescriptorPool((VkDevice)device, pool, NULL);
	memset(&peer.objects[sampler->wire_id], 0, sizeof(peer.objects[sampler->wire_id]));
	vulkan_object_free(sampler);
	memset(&peer.objects[view->wire_id], 0, sizeof(peer.objects[view->wire_id]));
	vulkan_object_free(view);
	memset(&peer.objects[buffer->wire_id], 0, sizeof(peer.objects[buffer->wire_id]));
	vulkan_object_free(buffer);
	memset(&peer.objects[texel->wire_id], 0, sizeof(peer.objects[texel->wire_id]));
	vulkan_object_free(texel);

	/* Succeeded: ignored fields remained untouched even after their source layout was destroyed. */
	return;
}

/* Makes a failed void destroy terminal while consuming the public object's local ownership. */
static void
test_destroy_loss(void)
{
	struct vulkan_context context;
	struct VkDevice_T device;
	struct callback_state creator;
	struct callback_state destroyer;
	VkAllocationCallbacks create_callbacks;
	VkAllocationCallbacks destroy_callbacks;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayout layout;
	VkDescriptorPool pool;
	VkDescriptorSet sets[2];
	uint64_t native_id;
	uint32_t opcode;
	uint32_t calls;
	uint32_t case_index;
	uint32_t index;
	VkResult status;

	/* Layout and pool destruction independently exercise callback OOM before native submission. */
	for (case_index = 0U; case_index < 2U; case_index++) {
		memset(&context, 0, sizeof(context));
		memset(&device, 0, sizeof(device));
		device.object.context = &context;
		device.object.wire_id = TEST_DEVICE_ID;

		/* Compatible callback policies keep creator accounting separate from destroy-time temporary allocation. */
		memset(&creator, 0, sizeof(creator));
		memset(&destroyer, 0, sizeof(destroyer));
		callbacks_init(&create_callbacks, &creator);
		callbacks_init(&destroy_callbacks, &destroyer);

		/* One ordinary sampled-image binding requires no external resource identity to create its layout. */
		memset(&binding, 0, sizeof(binding));
		binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		binding.descriptorCount = 1U;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		layout = create_layout(&device, &binding, 1U, &create_callbacks);
		pool = VK_NULL_HANDLE;
		native_id = wire_id((uint64_t)(uintptr_t)layout);
		opcode = 73U;

		/* Pool destruction must also consume local set snapshots when its native operation cannot be sent. */
		if (case_index != 0U) {
			pool = create_pool(&device, &create_callbacks);
			status = allocate_sets(&device, pool, layout, 2U, sets);
			assert(status == VK_SUCCESS);
			native_id = wire_id((uint64_t)(uintptr_t)pool);
			opcode = 75U;
		}

		/* Fail only the actual destruction writer, before the peer can consume any native object. */
		calls = peer.calls[opcode];
		destroyer.fail_command = 1U;
		if (case_index == 0U) {
			vkDestroyDescriptorSetLayout(&device, layout, &destroy_callbacks);
		} else {
			vkDestroyDescriptorPool(&device, pool, &destroy_callbacks);
		}

		/* The public handle is consumed, but the unknown remote lifetime cannot masquerade as a healthy context. */
		assert(context.error == VK_ERROR_DEVICE_LOST);
		assert(peer.calls[opcode] == calls);
		assert(peer.objects[native_id].kind != 0U);

		/* A still-local source layout can be released without issuing more native work in the lost context. */
		if (case_index != 0U)
			vkDestroyDescriptorSetLayout(&device, layout, &destroy_callbacks);

		/* All callback-owned local state is gone even though terminal namespace close still owns native state. */
		assert(device.object.first_child == NULL);
		assert(creator.live == 0U);
		assert(destroyer.live == 0U);

		/* Simulate renderer namespace close, which consumes all uncertain IDs together after terminal loss. */
		for (index = 0U; index < TEST_OBJECTS; index++)
			memset(&peer.objects[index], 0, sizeof(peer.objects[index]));
	}

	/* Succeeded: both void destruction paths refuse continued use after an unsent native cleanup. */
	return;
}
