/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns native Vulkan allocations and true shared, coherent CPU mappings.
 */

#include "internal.h"

#include <uapi/gpu.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define VULKAN_MEMORY_PAGE	4096U

/*
 * One native allocation exports at most one persistent renderer blob.
 * Unmap retires the user view, while the blob survives for later map calls.
 */
struct memory_allocation {
	struct vulkan_memory memory;
	uint64_t blob;
	uint64_t token;
	size_t view_bytes;
};

static VkResult memory_export(struct VkDevice_T *device, struct memory_allocation *allocation, uint64_t bytes);
static VkResult memory_unmap(struct VkDevice_T *device, struct memory_allocation *allocation);
static void memory_native_free(struct VkDevice_T *device, struct vulkan_memory *memory);
static void memory_lost(struct VkDevice_T *device);
static VkResult memory_ranges(struct VkDevice_T *device, uint32_t count, const VkMappedMemoryRange *ranges);

/*
 * Allocates native memory before publishing its standard, process-local handle.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(
	VkDevice device,
	const VkMemoryAllocateInfo *pAllocateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDeviceMemory *pMemory)
{
	struct VkDevice_T *owner;
	struct memory_allocation *allocation;
	struct vulkan_memory *memory;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	VkMemoryPropertyFlags properties;
	uint64_t native_bytes;
	uint64_t present;
	uint64_t identifier;
	VkResult status;
	VkResult cleanup;

	/* Invalid or failed allocation never exposes an unowned handle. */
	if (pMemory == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	*pMemory = VK_NULL_HANDLE;

	/* Resolve the device before interpreting its advertised memory types. */
	owner = vulkan_device(device);
	if (owner == NULL ||
	    pAllocateInfo == NULL ||
	    pAllocateInfo->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
	    pAllocateInfo->allocationSize == 0 ||
	    pAllocateInfo->memoryTypeIndex >= owner->physical->memory.memoryTypeCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A lost renderer namespace cannot accept a new allocation. */
	status = __atomic_load_n(&owner->object.context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* CPU-visible allocations reserve whole pages without changing public bounds. */
	properties = owner->physical->memory.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
	native_bytes = pAllocateInfo->allocationSize;
	if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
		/* Noncoherent types are filtered from the advertised host-visible set. */
		if ((properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
			return VK_ERROR_FEATURE_NOT_PRESENT;

		/* Page rounding must not wrap the renderer allocation size. */
		if (native_bytes > UINT64_MAX - (VULKAN_MEMORY_PAGE - 1U))
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;

		/* Keep both the exported blob and the eventual user view within local limits. */
		native_bytes = (native_bytes + VULKAN_MEMORY_PAGE - 1U) &
		    ~(uint64_t)(VULKAN_MEMORY_PAGE - 1U);
		if (native_bytes > owner->object.context->max_resource_bytes || native_bytes > SIZE_MAX)
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/* The allocation's effective callbacks also own its command temporaries. */
	status = vulkan_object_alloc(
		sizeof(*allocation),
		sizeof(uint64_t),
		VULKAN_OBJECT_DEVICE_MEMORY,
		&owner->object,
		owner->object.context,
		pAllocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (status != VK_SUCCESS)
		return status;

	/* Preserve application bounds separately from the page-rounded native size. */
	allocation = (struct memory_allocation *)object;
	memory = &allocation->memory;
	memory->bytes = pAllocateInfo->allocationSize;
	memory->type_index = pAllocateInfo->memoryTypeIndex;
	memory->properties = properties;
	memory->backing_private = allocation;

	/* Reserve the renderer identity before emitting any creation request. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Leave without publishing an allocation the renderer did not accept. */
		return status;
	}

	/* Only protocol values cross the wire; application allocator pointers stay local. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkAllocateMemory);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_pointer(&writer, pAllocateInfo);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	vulkan_write_pointer(&writer, NULL);
	vulkan_write_u64(&writer, native_bytes);
	vulkan_write_u32(&writer, pAllocateInfo->memoryTypeIndex);
	vulkan_write_pointer(&writer, NULL);
	vulkan_write_pointer(&writer, pMemory);
	vulkan_write_u64(&writer, object->wire_id);
	status = vulkan_command_execute(object->context, &writer, 24U, &reply, VK_TRUE);
	vulkan_writer_finish(&writer);

	/* Successful creation must echo exactly the identity reserved by this process. */
	if (status == VK_SUCCESS) {
		present = vulkan_read_u64(&reply);
		identifier = vulkan_read_u64(&reply);
		if (reply.error != VK_SUCCESS ||
		    present != 1U ||
		    identifier != object->wire_id) {
			memory_lost(owner);
			status = VK_ERROR_DEVICE_LOST;
		}
	}

	/* Retire reply storage before unwinding a failed native allocation. */
	vulkan_reader_finish(&reply);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Leave without publishing an allocation the renderer did not accept. */
		return status;
	}

	/* Export once at allocation so later map/unmap cycles reuse the same renderer blob. */
	if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
		/* Prepare the persistent export before any application can resolve the handle. */
		status = memory_export(owner, allocation, native_bytes);
		if (status != VK_SUCCESS) {
			memory_native_free(owner, memory);
			vulkan_object_free(object);

			/* Report the export failure after releasing the native allocation. */
			return status;
		}
	}

	/* Publish only after native storage and any required shared blob exist. */
	status = vulkan_object_publish(object);
	if (status != VK_SUCCESS) {
		/* Unpublished storage still owns any blob created during preparation. */
		if (allocation->blob != 0) {
			vulkan_context_lock(object->context);

			cleanup = vulkan_resource_destroy(object->context, allocation->blob);

			vulkan_context_unlock(object->context);

			/* Failed blob retirement forbids a later native free in this uncertain namespace. */
			if (cleanup != VK_SUCCESS) {
				memory_lost(owner);
				status = VK_ERROR_DEVICE_LOST;
			}
		}

		/* The native allocation can retire after its separate blob is gone. */
		memory_native_free(owner, memory);
		vulkan_object_free(object);

		/* Leave without publishing an allocation the renderer did not accept. */
		return status;
	}

	/* Succeeded: API bounds describe the requested allocation, including partial pages. */
	*pMemory = (VkDeviceMemory)(uintptr_t)vulkan_nondispatchable_handle(object);
	return VK_SUCCESS;
}

/*
 * Releases a mapped allocation only after its user view has actually retired.
 */
VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(
	VkDevice device,
	VkDeviceMemory memory,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_memory *storage;
	struct memory_allocation *allocation;
	struct vulkan_context *context;
	VkResult status;

	/* Vulkan permits null memory and implicitly unmaps a live allocation on free. */
	if (memory == VK_NULL_HANDLE)
		return;

	/* Resolve the allocation within the device that owns its renderer identity. */
	owner = vulkan_device(device);
	storage = vulkan_memory(memory);
	if (owner == NULL ||
	    storage == NULL ||
	    storage->object.parent != &owner->object)
		return;

	/* This destruction command supplies the effective userdata for both temporaries and final storage. */
	if (pAllocator != NULL) {
		storage->object.allocator.callbacks = *pAllocator;
		storage->object.allocator.has_callbacks = VK_TRUE;
	}

	/* An implicit unmap must succeed before either backing allocation is destroyed. */
	allocation = storage->backing_private;
	status = memory_unmap(owner, allocation);
	if (status != VK_SUCCESS)
		return;

	/* Blob retirement precedes native memory destruction and consumes no user pointers. */
	context = owner->object.context;
	if (allocation->blob != 0) {
		vulkan_context_lock(context);

		status = vulkan_resource_destroy(context, allocation->blob);

		vulkan_context_unlock(context);

		/* Blob destruction failure invalidates the renderer namespace. */
		if (status != VK_SUCCESS)
			memory_lost(owner);
		allocation->blob = 0;
	}

	/* Remove both the native allocation and its local callback-owned handle. */
	memory_native_free(owner, storage);
	vulkan_object_unpublish(&storage->object);
	vulkan_object_free_with_allocator(&storage->object, pAllocator);

	/* Succeeded: the allocation has no public handle or active user view. */
	return;
}

/*
 * Returns shared GPU storage rather than a copied staging allocation.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(
	VkDevice device,
	VkDeviceMemory memory,
	VkDeviceSize offset,
	VkDeviceSize size,
	VkMemoryMapFlags flags,
	void **ppData)
{
	struct VkDevice_T *owner;
	struct vulkan_memory *storage;
	struct memory_allocation *allocation;
	struct vulkan_context *context;
	void *address;
	VkResult status;

	/* Failed mapping preserves both allocation ownership and the caller's output slot. */
	if (ppData == NULL)
		return VK_ERROR_MEMORY_MAP_FAILED;
	*ppData = NULL;

	/* Resolve the allocation within the device that owns its renderer identity. */
	owner = vulkan_device(device);
	storage = vulkan_memory(memory);
	if (owner == NULL ||
	    storage == NULL ||
	    storage->object.parent != &owner->object)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* Only a previously unmapped host-visible allocation permits a CPU view. */
	if ((storage->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
	    storage->mapped_base != NULL ||
	    flags != 0 ||
	    offset >= storage->bytes)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* Expand the sentinel within application bounds before checking the requested range. */
	if (size == VK_WHOLE_SIZE)
		size = storage->bytes - offset;

	/* Reject empty or overflowing views without changing allocation state. */
	if (size == 0 || size > storage->bytes - offset)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* A previous transport loss also prevents new CPU mappings. */
	context = owner->object.context;
	status = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;
	allocation = storage->backing_private;

	/* Mapping the whole allocation keeps pData-offset aligned for every valid offset. */
	vulkan_context_lock(context);

	address = mmap(
		NULL,
		allocation->view_bytes,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		context->fd,
		(off_t)allocation->token);

	vulkan_context_unlock(context);

	/* A failed VM insertion leaves the persistent renderer export available for retry. */
	if (address == MAP_FAILED)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* Record the active application range while retaining the whole-allocation VM view. */
	storage->mapped_base = address;
	storage->mapped_offset = offset;
	storage->mapped_bytes = size;
	*ppData = (uint8_t *)address + (size_t)offset;

	/* Succeeded: CPU and GPU address the same coherent backing until unmap. */
	return VK_SUCCESS;
}

/*
 * Retires only the active user view, keeping the exported allocation reusable.
 */
VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(
	VkDevice device,
	VkDeviceMemory memory)
{
	struct VkDevice_T *owner;
	struct vulkan_memory *storage;
	VkResult status;

	/* Resolve the allocation within the device that owns its renderer identity. */
	owner = vulkan_device(device);
	storage = vulkan_memory(memory);
	if (owner == NULL ||
	    storage == NULL ||
	    storage->object.parent != &owner->object)
		return;

	/* Retire the view through the same path used by implicit unmapping on free. */
	status = memory_unmap(owner, storage->backing_private);
	if (status != VK_SUCCESS)
		return;

	/* Succeeded: a later map may reuse the persistent export. */
	return;
}

/*
 * Coherent memory requires ordering but no unavailable renderer cache-flush command.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkFlushMappedMemoryRanges(
	VkDevice device,
	uint32_t memoryRangeCount,
	const VkMappedMemoryRange *pMemoryRanges)
{
	struct VkDevice_T *owner;
	VkResult status;

	/* Validate every range before establishing the host write ordering boundary. */
	owner = vulkan_device(device);
	status = memory_ranges(owner, memoryRangeCount, pMemoryRanges);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: validated coherent ranges now cross a CPU ordering boundary. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return VK_SUCCESS;
}

/*
 * Observes coherent device writes after the application's GPU synchronization.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkInvalidateMappedMemoryRanges(
	VkDevice device,
	uint32_t memoryRangeCount,
	const VkMappedMemoryRange *pMemoryRanges)
{
	struct VkDevice_T *owner;
	VkResult status;

	/* This operation does not substitute for a fence or queue completion wait. */
	owner = vulkan_device(device);
	status = memory_ranges(owner, memoryRangeCount, pMemoryRanges);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: validated coherent ranges now cross a CPU ordering boundary. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return VK_SUCCESS;
}

/*
 * Queries the renderer's actual commitment for lazily allocated device memory.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceMemoryCommitment(
	VkDevice device,
	VkDeviceMemory memory,
	VkDeviceSize *pCommittedMemoryInBytes)
{
	struct VkDevice_T *owner;
	struct vulkan_memory *storage;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	uint64_t present;
	uint64_t bytes;
	VkResult status;

	/* An unavailable native reply cannot expose uninitialized local bytes. */
	if (pCommittedMemoryInBytes == NULL)
		return;
	*pCommittedMemoryInBytes = 0;

	/* Resolve the allocation within the device that owns its renderer identity. */
	owner = vulkan_device(device);
	storage = vulkan_memory(memory);
	if (owner == NULL ||
	    storage == NULL ||
	    storage->object.parent != &owner->object)
		return;

	/* Query commitment through the native allocation without changing its CPU view. */
	vulkan_writer_init_for_object(&writer, &storage->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetDeviceMemoryCommitment);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, storage->object.wire_id);
	vulkan_write_pointer(&writer, pCommittedMemoryInBytes);
	status = vulkan_command_execute(owner->object.context, &writer, 20U, &reply, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* A valid commitment reply supplies one size within the allocation bounds. */
	if (status == VK_SUCCESS) {
		present = vulkan_read_u64(&reply);
		bytes = vulkan_read_u64(&reply);
		if (reply.error != VK_SUCCESS ||
		    present != 1U ||
		    bytes > storage->bytes) {
			memory_lost(owner);
			status = VK_ERROR_DEVICE_LOST;
		} else {
			*pCommittedMemoryInBytes = bytes;
		}
	}

	/* A failed query leaves the initialized zero output after releasing reply ownership. */
	vulkan_reader_finish(&reply);
	if (status != VK_SUCCESS)
		return;

	/* Succeeded: the caller receives the native allocation commitment. */
	return;
}

/* Exports one renderer allocation while serializing all native operations on its fd. */
static VkResult
memory_export(
	struct VkDevice_T *device,
	struct memory_allocation *allocation,
	uint64_t bytes)
{
	struct vulkan_context *context;
	struct gpu_resource_map request;
	uint32_t resource;
	VkResult status;
	VkResult cleanup;
	int error;

	/* A physical device must not advertise host visibility without shared mapping. */
	context = device->object.context;
	if ((context->capabilities & GPU_CAP_MAPPING) == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Export and discover the mmap token as one serialized session transaction. */
	vulkan_context_lock(context);

	status = vulkan_resource_blob(
		context,
		bytes,
		allocation->memory.object.wire_id,
		&allocation->blob,
		&resource);
	if (status != VK_SUCCESS) {
		vulkan_context_unlock(context);
		return status;
	}

	/* Obtain an opaque session offset; physical and kernel virtual addresses stay private. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = allocation->blob;
	error = ioctl(context->fd, GPU_RESOURCE_MAP, &request);
	if (error != 0) {
		cleanup = vulkan_resource_destroy(context, allocation->blob);
		allocation->blob = 0;
		vulkan_context_unlock(context);

		/* An uncertain live blob must remain context-owned instead of losing its native memory underneath. */
		if (cleanup != VK_SUCCESS) {
			memory_lost(device);
			return VK_ERROR_DEVICE_LOST;
		}

		/* Only a completely retired export permits ordinary native-allocation rollback. */
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/* The kernel token must describe one complete page-aligned export. */
	if (request.offset == 0 ||
	    request.offset > INT64_MAX ||
	    request.bytes != bytes ||
	    (request.offset & (VULKAN_MEMORY_PAGE - 1U)) != 0) {
		(void)vulkan_resource_destroy(context, allocation->blob);
		allocation->blob = 0;
		vulkan_context_unlock(context);
		memory_lost(device);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Keep the opaque token for later maps without retaining physical addresses. */
	allocation->token = request.offset;
	allocation->view_bytes = (size_t)request.bytes;

	vulkan_context_unlock(context);

	/* Succeeded: the allocation owns one persistent, reusable renderer export. */
	return VK_SUCCESS;
}

/* Retires the actual VM mapping before discarding its local address and bounds. */
static VkResult
memory_unmap(
	struct VkDevice_T *device,
	struct memory_allocation *allocation)
{
	struct vulkan_memory *memory;
	int error;

	/* Freeing an allocation without an active view needs no VM operation. */
	memory = &allocation->memory;
	if (memory->mapped_base == NULL)
		return VK_SUCCESS;

	/* Publish host writes before retiring the mapping that makes them addressable. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	error = munmap(memory->mapped_base, allocation->view_bytes);
	if (error != 0) {
		memory_lost(device);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Clear public mapping bounds only after the VM accepted the unmap. */
	memory->mapped_base = NULL;
	memory->mapped_offset = 0;
	memory->mapped_bytes = 0;

	/* Succeeded: the allocation remains owned and can be mapped again. */
	return VK_SUCCESS;
}

/* Frees only the native Vulkan allocation, after its separate blob has retired. */
static void
memory_native_free(
	struct VkDevice_T *device,
	struct vulkan_memory *memory)
{
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	VkResult status;

	/* Application allocator callbacks never cross the renderer boundary. */
	vulkan_writer_init_for_object(&writer, &memory->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkFreeMemory);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, memory->object.wire_id);
	vulkan_write_pointer(&writer, NULL);
	status = vulkan_command_execute(device->object.context, &writer, 4U, &reply, VK_FALSE);
	vulkan_reader_finish(&reply);
	vulkan_writer_finish(&writer);

	/* Failed native destruction makes later device calls observe a lost namespace. */
	if (status != VK_SUCCESS) {
		memory_lost(device);
		return;
	}

	/* Succeeded: no native allocation remains behind this local handle. */
	return;
}

/* Publishes a terminal transport error for subsequent result-returning API calls. */
static void
memory_lost(
	struct VkDevice_T *device)
{
	/* Every family shares the context error, including calls using another device handle. */
	__atomic_store_n(&device->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
	__atomic_store_n(&device->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* Succeeded: future calls cannot continue a damaged renderer namespace. */
	return;
}

/* Checks mapped ranges without pretending to support unadvertised noncoherent memory. */
static VkResult
memory_ranges(
	struct VkDevice_T *device,
	uint32_t count,
	const VkMappedMemoryRange *ranges)
{
	struct vulkan_memory *memory;
	VkDeviceSize bytes;
	uint32_t index;
	VkResult status;

	/* Device loss and absent arrays are reported before any range is inspected. */
	if (device == NULL ||
	    (count != 0 && ranges == NULL))
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* Cached visibility is insufficient after the renderer context is lost. */
	status = __atomic_load_n(&device->object.context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* Validate all ranges before the caller establishes any cache-ordering boundary. */
	for (index = 0; index < count; index++) {
		/* Each range must resolve to an active mapping owned by this device. */
		memory = vulkan_memory(ranges[index].memory);
		if (memory == NULL ||
		    memory->object.parent != &device->object ||
		    memory->mapped_base == NULL ||
		    ranges[index].sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE)
			return VK_ERROR_MEMORY_MAP_FAILED;

		/* A noncoherent type cannot be handled by the advertised coherent-only policy. */
		if ((memory->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
			return VK_ERROR_FEATURE_NOT_PRESENT;

		/* The starting byte must belong to both the allocation and its current view. */
		if (ranges[index].offset < memory->mapped_offset || ranges[index].offset >= memory->bytes)
			return VK_ERROR_MEMORY_MAP_FAILED;

		/* Expand the whole-allocation sentinel before evaluating the final view bound. */
		bytes = ranges[index].size;
		if (bytes == VK_WHOLE_SIZE)
			bytes = memory->bytes - ranges[index].offset;

		/* Subtractions are safe only after their corresponding lower bounds passed. */
		if (bytes == 0 ||
		    bytes > memory->bytes - ranges[index].offset ||
		    ranges[index].offset - memory->mapped_offset > memory->mapped_bytes ||
		    bytes > memory->mapped_bytes - (ranges[index].offset - memory->mapped_offset))
			return VK_ERROR_MEMORY_MAP_FAILED;
	}

	/* Succeeded: every range lies inside coherent mapped storage. */
	return VK_SUCCESS;
}
