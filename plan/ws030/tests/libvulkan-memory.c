/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Tests real allocation and wire ownership against an independent peer and shared file mappings. */

#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <uapi/gpu.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* One native allocation retains its exported file until mapping and blob ownership are released. */
struct native_memory {
	uint64_t id;
	uint64_t bytes;
	uint64_t token;
	int fd;
	unsigned blob;
	unsigned mapped;
	unsigned exported;
};

/* The single-threaded peer owns these records until each real vkFreeMemory request arrives. */
static struct native_memory native[256];

/* The fixture's logical device owns every real local memory object until its public free. */
static struct VkDevice_T owner;

/* Three native memory types distinguish coherent mappings, device storage and lazy commitment. */
static struct VkPhysicalDevice_T physical;

/* Raw blob and mapping operations serialize on this mock session's actual mutex. */
static struct vulkan_context context;

/* Each export failure request is consumed by the next mocked blob creation. */
static unsigned fail_blob;

/* Each token-query failure is consumed before an offset is published to the caller. */
static unsigned fail_ioctl;

/* Each mapping failure leaves the persistent native export available for an ordinary retry. */
static unsigned fail_mmap;

/* Each native allocation failure refuses ownership before a remote record is created. */
static unsigned fail_native;

/* A failed blob rollback leaves its actual exported file owned by terminal session cleanup. */
static unsigned fail_destroy;

/* A zero live count means every independently recorded remote allocation was freed. */
static unsigned live;

/* Export count proves repeated vkMapMemory calls reuse the allocation's persistent native blob. */
static unsigned exports;

/* Successful shared mappings are counted independently of failed mapping attempts. */
static unsigned mappings;

/* Native frees prove both rollback and ordinary destruction reached the peer. */
static unsigned frees;

/* Application allocations remain balanced with callback frees at complete fixture cleanup. */
static unsigned callback_allocs;

/* This count includes command temporaries and local allocation objects released by callbacks. */
static unsigned callback_frees;

/* This historical counter records compatible final-destroy userdata instead of creation userdata. */
static unsigned destroy_userdata_frees;

/* Destruction-command temporary allocations must use that command's compatible userdata. */
static unsigned destroy_command_allocs;

/* Creation callbacks use this stable identity throughout the serial fixture. */
static int create_userdata;

/* Compatible destruction callbacks use a distinct identity with the same allocation mechanics. */
static int destroy_userdata;

static uint32_t get32(const uint8_t *p);
static uint64_t get64(const uint8_t *p);
static void put32(uint8_t *p, uint32_t n);
static void put64(uint8_t *p, uint64_t n);
static struct native_memory *find(uint64_t id);
static void locked(void);
static void *allocate(void *data, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void release(void *data, void *pointer);
static void *reallocate(void *data, void *old, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void test_rollback_loss(const VkAllocationCallbacks *callbacks);

/*
 * Takes the mock session mutex through the production raw-operation contract.
 */
void
vulkan_context_lock(
	struct vulkan_context *ctx)
{
	int error;

	/* Serialize raw blob and mmap operations on this session. */
	error = pthread_mutex_lock(&ctx->mutex);
	assert(error == 0);

	/* Succeeded: the raw transaction has exclusive session ownership. */
	return;
}

/*
 * Returns exclusive mock-session ownership to the next raw operation.
 */
void
vulkan_context_unlock(
	struct vulkan_context *ctx)
{
	int error;

	/* Release the transaction only after its mocked fd operation completes. */
	error = pthread_mutex_unlock(&ctx->mutex);
	assert(error == 0);

	/* Succeeded: no raw operation remains inside this critical section. */
	return;
}

/*
 * Interprets real allocation, free and commitment requests with independent wire offsets.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *ctx,
	const struct vulkan_writer *writer,
	size_t capacity,
	struct vulkan_reader *reply)
{
	uint32_t opcode;
	uint32_t word;
	uint64_t id;
	uint64_t value;
	unsigned i;
	struct native_memory *memory;
	VkResult status;

	/* The real context contract refuses new native work after any family declares namespace loss. */
	status = __atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* The complete native header identifies the intended device and synchronous command format. */
	assert(ctx == &context);
	assert(writer->bytes >= 16);
	word = get32(writer->data + 4);
	assert(word == 1);
	value = get64(writer->data + 8);
	assert(value == owner.object.wire_id);
	memset(reply, 0, sizeof(*reply));
	reply->allocator = writer->allocator;

	/* Reply storage follows the effective callback policy used by the actual command writer. */
	if (reply->allocator.has_callbacks) {
		reply->data = vulkan_allocate(&reply->allocator, capacity, 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		assert(reply->data != NULL);
	} else {
		reply->data = malloc(capacity);
		assert(reply->data != NULL);
	}

	/* Every response begins with the same independently decoded native opcode. */
	memset(reply->data, 0, capacity);
	opcode = get32(writer->data);
	put32(reply->data, opcode);

	/* Allocation checks the standard structure and reserves remote ownership only on success. */
	if (opcode == VULKAN_OPCODE_vkAllocateMemory) {
		assert(writer->bytes == 72);
		value = get64(writer->data + 16);
		assert(value == 1);
		word = get32(writer->data + 24);
		assert(word == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
		value = get64(writer->data + 28);
		assert(value == 0);
		value = get64(writer->data + 48);
		assert(value == 0);
		value = get64(writer->data + 56);
		assert(value == 1);
		assert(capacity >= 24);
		reply->bytes = 24;

		/* A native OOM response creates no allocation that the caller would need to unwind. */
		if (fail_native) {
			fail_native = 0;
			put32(reply->data + 4, (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY);
			return VK_SUCCESS;
		}

		/* Locate the next empty peer record without imposing a small production resource quota. */
		for (i = 0; i < 256; i++) {
			if (native[i].id == 0)
				break;
		}

		/* A native allocation retains one page-token identity before any file export exists. */
		assert(i < 256);
		memory = &native[i];
		memory->id = get64(writer->data + 64);
		memory->bytes = get64(writer->data + 36);
		memory->fd = -1;
		memory->token = (memory->id + 1) * 4096;

		/* The returned ID becomes one live native allocation requiring a later explicit free. */
		live++;
		put64(reply->data + 8, 1);
		put64(reply->data + 16, memory->id);
	} else if (opcode == VULKAN_OPCODE_vkFreeMemory) {
		/* Native memory must outlive both its exported blob and every shared mapping. */
		assert(writer->bytes == 32);
		value = get64(writer->data + 24);
		assert(value == 0);
		id = get64(writer->data + 16);
		memory = find(id);
		assert(memory->blob == 0);
		assert(memory->mapped == 0);
		memset(memory, 0, sizeof(*memory));

		/* Final remote free removes the record from the fixture's independently tracked live namespace. */
		live--;
		frees++;
		reply->bytes = 4;
	} else {
		/* Lazy commitment is a native output, not a value inferred from application allocation size. */
		assert(opcode == VULKAN_OPCODE_vkGetDeviceMemoryCommitment);
		assert(writer->bytes == 32);
		value = get64(writer->data + 24);
		assert(value == 1);
		id = get64(writer->data + 16);
		memory = find(id);
		assert(capacity >= 20);
		put64(reply->data + 4, 1);
		put64(reply->data + 12, memory->bytes / 2);
		reply->bytes = 20;
	}

	/* Succeeded: the actual response decoder receives a complete independent native result. */
	return VK_SUCCESS;
}

/*
 * Exports one native allocation into an unlinked shared file while holding the raw session lock.
 */
VkResult
vulkan_resource_blob(
	struct vulkan_context *ctx,
	uint64_t bytes,
	uint64_t id,
	uint64_t *handle,
	uint32_t *resource)
{
	struct native_memory *memory;
	char name[] = "/tmp/zedbsd-vulkan-memory.XXXXXX";
	int error;

	/* Blob operations must remain serialized with token query and mapping operations. */
	locked();
	assert(ctx == &context);

	/* Export failure leaves the native memory owner for the real rollback path to free. */
	if (fail_blob) {
		fail_blob = 0;
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/* The actual allocation is exported only once, with its page-rounded native size. */
	memory = find(id);
	assert(memory->exported == 0);
	assert(memory->bytes == bytes);
	assert(bytes % 4096 == 0);
	memory->fd = mkstemp(name);
	assert(memory->fd >= 0);
	error = unlink(name);
	assert(error == 0);
	error = ftruncate(memory->fd, (off_t)bytes);
	assert(error == 0);

	/* The persistent blob retains its file until the real resource-destroy path runs. */
	memory->blob = 1;
	memory->exported = 1;
	exports++;
	*handle = id;
	*resource = (uint32_t)id;

	/* Succeeded: the native allocation has one reusable shared-file export. */
	return VK_SUCCESS;
}

/*
 * Releases an exported file only after all mapped views have been removed.
 */
VkResult
vulkan_resource_destroy(
	struct vulkan_context *ctx,
	uint64_t handle)
{
	struct native_memory *memory;
	int error;

	/* Native identity lookup precedes the raw-session ownership check as in the original fixture. */
	memory = find(handle);
	locked();
	assert(ctx == &context);
	assert(memory->blob != 0);
	assert(memory->mapped == 0);

	/* Failed destruction preserves the native blob and file for the session-close path. */
	if (fail_destroy != 0) {
		fail_destroy = 0;
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/* Complete blob retirement releases the shared file before native memory can be freed. */
	error = close(memory->fd);
	assert(error == 0);

	/* Consuming blob ownership permits the subsequent native memory free. */
	memory->blob = 0;
	memory->fd = -1;

	/* Succeeded: this native allocation retains no open exported file. */
	return VK_SUCCESS;
}

/*
 * Supplies page-aligned mmap tokens through the real public GPU mapping request shape.
 */
int
memory_test_ioctl(
	int fd,
	unsigned long command,
	...)
{
	va_list ap;
	struct gpu_resource_map *request;
	struct native_memory *memory;

	/* Mapping-token queries remain inside the same session transaction as blob export. */
	locked();
	assert(fd == context.fd);
	assert(command == GPU_RESOURCE_MAP);
	va_start(ap, command);
	request = va_arg(ap, struct gpu_resource_map *);
	va_end(ap);

	/* A failed token query must trigger the real allocation rollback without publishing an offset. */
	if (fail_ioctl) {
		fail_ioctl = 0;
		errno = ENOSPC;
		return -1;
	}

	/* The kernel ABI receives a complete input record with no preselected mapping window. */
	assert(request->version == GPU_ABI_VERSION);
	assert(request->size == sizeof(*request));
	assert(request->offset == 0);
	assert(request->bytes == 0);
	memory = find(request->handle);
	assert(memory->blob != 0);
	request->offset = memory->token;
	request->bytes = memory->bytes;

	/* Succeeded: the caller can map only this allocation's independently assigned token. */
	return 0;
}

/*
 * Maps the native shared file while checking exact protection and token semantics.
 */
void *
memory_test_mmap(
	void *hint,
	size_t bytes,
	int prot,
	int flags,
	int fd,
	off_t offset)
{
	struct native_memory *memory;
	unsigned i;
	void *address;

	/* Actual vkMapMemory requests a coherent shared read/write mapping inside the raw session lock. */
	locked();
	assert(hint == NULL);
	assert(fd == context.fd);
	assert(prot == (PROT_READ | PROT_WRITE));
	assert(flags == MAP_SHARED);

	/* Mapping failure preserves the exported file so the next ordinary map can reuse it. */
	if (fail_mmap) {
		fail_mmap = 0;
		errno = ENOMEM;
		return MAP_FAILED;
	}

	/* Resolve the token independently from the caller's local memory handle. */
	for (i = 0; i < 256; i++) {
		if (native[i].token == (uint64_t)offset)
			break;
	}

	/* One active mapping covers the page-rounded export, not merely the application's requested subrange. */
	assert(i < 256);
	memory = &native[i];
	assert(bytes == memory->bytes);
	assert(memory->mapped == 0);
	assert(memory->blob != 0);
	address = mmap(NULL, bytes, prot, flags, memory->fd, 0);
	assert(address != MAP_FAILED);

	/* The shared view keeps the blob busy until the actual unmap path consumes it. */
	memory->mapped = 1;
	mappings++;

	/* Succeeded: file reads and writes can now observe the same bytes as the application pointer. */
	return address;
}

/*
 * Removes the current shared view without consuming its persistent blob export.
 */
int
memory_test_munmap(
	void *address,
	size_t bytes)
{
	struct native_memory *memory;
	unsigned i;
	int error;

	/* Tests retain one active map at a time, so its owner is unambiguous. */
	for (i = 0; i < 256; i++) {
		if (native[i].mapped != 0)
			break;
	}

	/* The full page-rounded view must be unmapped before releasing the exported file. */
	assert(i < 256);
	memory = &native[i];
	assert(bytes == memory->bytes);
	error = munmap(address, bytes);
	assert(error == 0);

	/* Removing mapping ownership permits either another map or final blob destruction. */
	memory->mapped = 0;

	/* Succeeded: the persistent export remains available without a stale active view. */
	return 0;
}

/*
 * Exercises allocation rollback, coherent bytes, bounded mappings and dynamic object ownership.
 */
int
main(void)
{
	VkMemoryAllocateInfo info;
	VkMappedMemoryRange range;
	VkAllocationCallbacks callbacks;
	VkAllocationCallbacks destroy_callbacks;
	VkDeviceMemory memory;
	VkDeviceMemory allocations[96];
	VkDeviceSize commitment;
	struct native_memory *backing;
	struct vulkan_memory *allocation;
	void *pointer;
	void *second;
	unsigned before;
	unsigned i;
	unsigned char byte;
	VkResult status;
	ssize_t transferred;
	int error;

	/* The fake session exposes mapping capability with the actual raw-operation mutex. */
	memset(&context, 0, sizeof(context));
	context.fd = 901;
	context.max_resource_bytes = 256U * 1024U * 1024U;
	context.capabilities = GPU_CAP_MAPPING;
	error = pthread_mutex_init(&context.mutex, NULL);
	assert(error == 0);

	/* Local allocation children belong to this ordinary logical device. */
	memset(&owner, 0, sizeof(owner));
	owner.object.context = &context;
	owner.object.wire_id = 1000;
	owner.object.kind = VULKAN_OBJECT_DEVICE;
	owner.physical = &physical;

	/* Native memory types preserve their original indices and distinguish lazy commitment. */
	memset(&physical, 0, sizeof(physical));
	physical.memory.memoryTypeCount = 3;
	physical.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	physical.memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	physical.memory.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

	/* An unaligned application size requires a larger page-rounded native export. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = 5000;

	/* Compatible allocation and destruction policies use different userdata identities. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.pUserData = &create_userdata;
	callbacks.pfnAllocation = allocate;
	callbacks.pfnReallocation = reallocate;
	callbacks.pfnFree = release;
	destroy_callbacks = callbacks;
	destroy_callbacks.pUserData = &destroy_userdata;

	/* Native allocation failure publishes neither a local handle nor a remote allocation. */
	fail_native = 1;
	status = vkAllocateMemory(&owner, &info, &callbacks, &memory);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(memory == VK_NULL_HANDLE);
	assert(live == 0);

	/* Export failure unwinds native memory and callback ownership together. */
	fail_blob = 1;
	status = vkAllocateMemory(&owner, &info, &callbacks, &memory);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(memory == VK_NULL_HANDLE);
	assert(live == 0);

	/* Token-query failure also consumes the partially exported allocation. */
	fail_ioctl = 1;
	status = vkAllocateMemory(&owner, &info, &callbacks, &memory);
	assert(status == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(memory == VK_NULL_HANDLE);
	assert(live == 0);

	/* Successful allocation retains application bounds separately from native page rounding. */
	status = vkAllocateMemory(&owner, &info, &callbacks, &memory);
	assert(status == VK_SUCCESS);
	assert(live == 1);
	allocation = vulkan_memory(memory);
	backing = find(allocation->object.wire_id);
	assert(backing->bytes == 8192);
	assert(allocation->bytes == 5000);
	before = exports;

	/* A failed mmap can be retried on the same persistent export without leaking a pointer. */
	fail_mmap = 1;
	status = vkMapMemory(&owner, memory, 1, 4097, 0, &pointer);
	assert(status == VK_ERROR_MEMORY_MAP_FAILED);
	assert(pointer == NULL);
	status = vkMapMemory(&owner, memory, 1, 4097, 0, &pointer);
	assert(status == VK_SUCCESS);
	assert(((uintptr_t)pointer - 1) % 4096 == 0);
	status = vkMapMemory(&owner, memory, 0, 1, 0, &second);
	assert(status == VK_ERROR_MEMORY_MAP_FAILED);

	/* Application stores become visible through the independent shared-file descriptor immediately. */
	((unsigned char *)pointer)[17] = 0x92;
	transferred = pread(backing->fd, &byte, 1, 18);
	assert(transferred == 1);
	assert(byte == 0x92);

	/* Native file writes become visible through the mapped application subrange without a private shadow copy. */
	byte = 0x6a;
	transferred = pwrite(backing->fd, &byte, 1, 18);
	assert(transferred == 1);
	assert(((unsigned char *)pointer)[17] == byte);

	/* Flush and invalidate honor the live application's mapped subrange bounds. */
	memset(&range, 0, sizeof(range));
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = memory;
	range.offset = 1;
	range.size = 4097;
	status = vkFlushMappedMemoryRanges(&owner, 1, &range);
	assert(status == VK_SUCCESS);
	status = vkInvalidateMappedMemoryRanges(&owner, 1, &range);
	assert(status == VK_SUCCESS);
	range.size = VK_WHOLE_SIZE;
	status = vkFlushMappedMemoryRanges(&owner, 1, &range);
	assert(status == VK_ERROR_MEMORY_MAP_FAILED);
	vkUnmapMemory(&owner, memory);

	/* Remapping the whole allocation retains both shared bytes and the original export identity. */
	status = vkMapMemory(&owner, memory, 0, VK_WHOLE_SIZE, 0, &pointer);
	assert(status == VK_SUCCESS);
	assert(((unsigned char *)pointer)[18] == 0x6a);
	assert(exports == before);
	status = vkMapMemory(&owner, memory, 5000, 1, 0, &second);
	assert(status == VK_ERROR_MEMORY_MAP_FAILED);
	vkFreeMemory(&owner, memory, &destroy_callbacks);
	assert(live == 0);
	assert(owner.object.first_child == NULL);
	assert(destroy_command_allocs != 0);
	assert(destroy_userdata_frees == destroy_command_allocs + 1U);
	assert(callback_allocs == callback_frees);

	/* Nonvisible lazy memory stays native, and commitment is decoded from its peer reply. */
	info.memoryTypeIndex = 2;
	info.allocationSize = 6000;
	status = vkAllocateMemory(&owner, &info, NULL, &memory);
	assert(status == VK_SUCCESS);
	status = vkMapMemory(&owner, memory, 0, VK_WHOLE_SIZE, 0, &pointer);
	assert(status == VK_ERROR_MEMORY_MAP_FAILED);
	vkGetDeviceMemoryCommitment(&owner, memory, &commitment);
	assert(commitment == 3000);
	vkFreeMemory(&owner, memory, NULL);

	/* Dynamic allocation ownership exceeds the former fixed resource table. */
	info.memoryTypeIndex = 1;
	info.allocationSize = 256;
	for (i = 0; i < 96; i++) {
		status = vkAllocateMemory(&owner, &info, NULL, &allocations[i]);
		assert(status == VK_SUCCESS);
	}

	/* Every accepted native allocation remains live until its individual public free. */
	assert(live == 96);

	/* Final free consumes all local children and their independently recorded native allocations. */
	for (i = 0; i < 96; i++)
		vkFreeMemory(&owner, allocations[i], NULL);

	/* No local or native owner remains after the complete mapping and dynamic-allocation scenarios. */
	assert(live == 0);
	assert(owner.object.first_child == NULL);
	assert(mappings == 2);
	assert(frees >= 100);

	/* Rollback must not free native memory beneath a blob whose destruction failed. */
	test_rollback_loss(&callbacks);
	error = pthread_mutex_destroy(&context.mutex);
	assert(error == 0);

	/* Succeeded: allocation, shared visibility, rollback and callback ownership obey the same ordinary API paths. */
	puts("libvulkan memory: wire, persistent export, actual shared bytes, bounds, rollback, callbacks, commitment and 96 allocations PASS");
	return 0;
}

/* Decodes one native little-endian word without calling the implementation's reader. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* Succeeded: the independent peer preserves every input byte's native significance. */
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Decodes the low and high words of one native 64-bit field independently. */
static uint64_t
get64(
	const uint8_t *p)
{
	uint32_t low;
	uint32_t high;

	/* Both halves preserve the protocol's explicit little-endian order. */
	low = get32(p);
	high = get32(p + 4);

	/* Succeeded: the peer can observe full-width IDs and allocation sizes. */
	return low | (uint64_t)high << 32;
}

/* Writes one native reply word without using the implementation's encoder. */
static void
put32(
	uint8_t *p,
	uint32_t n)
{
	unsigned i;

	/* Emit the reply in native little-endian byte order. */
	for (i = 0; i < 4; i++)
		p[i] = (uint8_t)(n >> (8 * i));

	/* Succeeded: the real decoder receives an independently constructed word. */
	return;
}

/* Writes a full-width native reply using separate low and high words. */
static void
put64(
	uint8_t *p,
	uint64_t n)
{
	/* Preserve the full native ID or size without host-structure alignment assumptions. */
	put32(p, (uint32_t)n);
	put32(p + 4, (uint32_t)(n >> 32));

	/* Succeeded: both halves are available to the actual native response decoder. */
	return;
}

/* Finds an existing native allocation without consulting the caller's local handle registry. */
static struct native_memory *
find(
	uint64_t id)
{
	unsigned i;

	/* Every accepted native ID must have exactly one peer-owned lifetime record. */
	for (i = 0; i < 256; i++) {
		if (native[i].id == id)
			break;
	}

	/* An unknown native ID is a fixture failure rather than an invented allocation. */
	if (i == 256) {
		assert(0);
		return NULL;
	}

	/* Succeeded: the requested ID still has its original live native ownership record. */
	return &native[i];
}

/* Confirms raw mocked operations execute inside the production session critical section. */
static void
locked(void)
{
	int error;

	/* A successful trylock would expose an operation outside its required session transaction. */
	error = pthread_mutex_trylock(&context.mutex);
	assert(error == EBUSY);

	/* Succeeded: another acquisition is refused because this raw operation already owns the session lock. */
	return;
}

/* Allocates callback-owned object or command storage with the requested alignment. */
static void *
allocate(
	void *data,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *pointer;
	int error;

	/* Creation callbacks must retain their application userdata and ordinary allocation scope. */
	pointer = NULL;
	assert(data == &create_userdata || data == &destroy_userdata);
	assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT || scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);

	/* Destroy-time callbacks must own command storage without being used to create a new memory object. */
	if (data == &destroy_userdata) {
		assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		destroy_command_allocs++;
	}

	/* The host allocator requires at least pointer alignment even for smaller Vulkan requests. */
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);

	/* Each accepted callback allocation must eventually have a matching ordinary callback free. */
	error = posix_memalign(&pointer, alignment, bytes);
	assert(error == 0);
	callback_allocs++;

	/* Succeeded: actual object and command ownership use application-provided storage. */
	return pointer;
}

/* Releases callback storage while observing compatible final-destroy userdata. */
static void
release(
	void *data,
	void *pointer)
{
	/* Both callback policies use the same allocation mechanics and remain compatible. */
	assert(data == &create_userdata || data == &destroy_userdata);

	/* NULL callback storage has no retained allocation to consume. */
	if (pointer == NULL)
		return;

	/* Final resource destruction must use the supplied compatible userdata rather than creation userdata. */
	if (data == &destroy_userdata)
		destroy_userdata_frees++;

	/* Releasing each callback pointer balances its one successful allocation. */
	callback_frees++;
	free(pointer);

	/* Succeeded: this callback allocation no longer owns host storage. */
	return;
}

/* Rejects a reallocation callback that these finite ordinary allocation commands never require. */
static void *
reallocate(
	void *data,
	void *old,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	/* These arguments belong to the standard callback signature, but any call is unexpected in this fixture. */
	(void)data;
	(void)old;
	(void)bytes;
	(void)alignment;
	(void)scope;
	assert(0);

	/* No replacement storage is supplied on this unreachable fixture-failure path. */
	return NULL;
}

/* Preserves uncertain blob ownership until context close after a failed mapping-token rollback. */
static void
test_rollback_loss(
	const VkAllocationCallbacks *callbacks)
{
	VkMemoryAllocateInfo info;
	VkDeviceMemory memory;
	VkResult status;
	unsigned before;
	unsigned index;
	unsigned retained;
	int error;

	/* Export succeeds, token query fails, and even explicit blob cleanup is refused once. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = 5000;
	before = frees;
	fail_ioctl = 1;
	fail_destroy = 1;
	status = vkAllocateMemory(&owner, &info, callbacks, &memory);
	assert(status == VK_ERROR_DEVICE_LOST);
	assert(memory == VK_NULL_HANDLE);
	assert(context.error == VK_ERROR_DEVICE_LOST);
	assert(owner.object.first_child == NULL);
	assert(callback_allocs == callback_frees);
	assert(frees == before);
	assert(live == 1);

	/* Session close, rather than a premature vkFreeMemory, consumes the still-exported native allocation. */
	retained = 0;
	for (index = 0; index < 256; index++) {
		if (native[index].id == 0)
			continue;

		/* The unresolved blob still owns its file and has never acquired a user mapping. */
		assert(native[index].blob == 1);
		assert(native[index].mapped == 0);
		error = close(native[index].fd);
		assert(error == 0);
		memset(&native[index], 0, sizeof(native[index]));

		/* Closing the mock namespace releases this last native owner without issuing a Vulkan command. */
		live--;
		retained++;
	}

	/* Succeeded: exactly one uncertain native allocation survived until terminal session cleanup. */
	assert(retained == 1);
	assert(live == 0);
	return;
}
