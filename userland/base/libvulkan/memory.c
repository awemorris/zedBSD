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
#include <uapi/gpu-scanout.h>

#include <uapi/gpu.h>
#include <uapi/gpu-allocation.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define VULKAN_MEMORY_PAGE	4096U
#define VULKAN_MEMORY_FD_SCHEMA	0x564d3031U

/* Immutable library metadata distinguishes compatible allocations from other kernel objects. */
struct memory_fd_metadata {
	uint64_t public_bytes;
	uint32_t memory_type;
	uint32_t reserved;
	uint8_t device_uuid[VK_UUID_SIZE];
	uint8_t driver_uuid[VK_UUID_SIZE];
};

/*
 * One native allocation owns at most one exported or imported renderer blob.
 * A CPU view may retire independently; the allocation retains its blob alias.
 */
struct memory_allocation {
	struct vulkan_memory memory;
	uint64_t blob;
	uint64_t token;
	size_t view_bytes;
	uint32_t external_type;
};

static VkResult memory_allocate(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkBool32 shared, uint32_t imported, const struct gpu_placement *placement, VkDeviceMemory *memory);
static VkResult memory_export(struct VkDevice_T *device, struct memory_allocation *allocation, uint64_t bytes, VkBool32 shared, const struct gpu_placement *placement);
static VkResult memory_unmap(struct VkDevice_T *device, struct memory_allocation *allocation);
static void memory_native_free(struct VkDevice_T *device, struct vulkan_memory *memory);
static void memory_lost(struct VkDevice_T *device);
static VkResult memory_ranges(struct VkDevice_T *device, uint32_t count, const VkMappedMemoryRange *ranges);
static VkResult memory_import_fd(struct VkDevice_T *device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, int fd, VkDeviceMemory *memory);
static VkResult memory_import_image_fd(struct VkDevice_T *device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, int fd, VkDeviceMemory *memory);
static VkResult memory_mapping_token(struct VkDevice_T *device, struct memory_allocation *allocation);

/*
 * Allocates native memory before publishing its standard, process-local handle.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(
	VkDevice device,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkDeviceMemory *memory)
{
	struct VkDevice_T *owner;
	struct vulkan_memory *storage;
	const VkBaseInStructure *next;
	const VkImportMemoryFdInfoKHR *import;
	const VkExportMemoryAllocateInfo *export;
	VkExternalMemoryHandleTypeFlags types;
	VkResult status;

	/* A failed public allocation never publishes an object or consumes an import fd. */
	if (memory == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Clear public ownership before resolving the allocation's device. */
	*memory = VK_NULL_HANDLE;
	owner = vulkan_device(device);
	if (owner == NULL || info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Standard export and import declarations are independent entries in the pNext chain. */
	import = NULL;
	export = NULL;
	for (next = info->pNext; next != NULL; next = next->pNext) {
		/* The import entry retains the caller's fd until native allocation succeeds. */
		if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR)
			import = (const VkImportMemoryFdInfoKHR *)next;

		/* Export permission is independent from an import elsewhere in the chain. */
		if (next->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
			export = (const VkExportMemoryAllocateInfo *)next;
	}

	/* An absent export declaration grants no later external fd export. */
	types = 0U;
	if (export != NULL)
		types = export->handleTypes;

	/* Guest external handles require explicit standard extension enablement. */
	if ((types != 0U ||
	     import != NULL) &&
	    !(owner->enabled_extensions & VULKAN_DEVICE_EXTERNAL_MEMORY_FD))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* The guest export interface supports only the reference-bearing opaque fd type. */
	if ((types & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) != 0U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Import requires that same handle type and an actual caller-owned descriptor. */
	if (import != NULL &&
	    (import->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
	     import->fd < 0))
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Import uses the retained allocation; export allocates native shareable storage once. */
	if (import != NULL)
		status = memory_import_fd(owner, info, allocator, import->fd, memory);
	else
		status = vulkan_memory_allocate(device, info, allocator, types != 0U, memory);
	if (status != VK_SUCCESS)
		return status;

	/* Re-export permission follows the public allocation declaration, including imported memory. */
	storage = vulkan_memory(*memory);
	storage->export_types = types;

	/* Succeeded: only a successful import consumes the caller's original fd. */
	return VK_SUCCESS;
}

/* Exports one reference to a compatible Vulkan allocation as a typed kernel handle fd. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetMemoryFdKHR(
	VkDevice device,
	const VkMemoryGetFdInfoKHR *pGetFdInfo,
	int *pFd)
{
	struct VkDevice_T *owner;
	struct vulkan_memory *memory;
	struct gpu_allocation_descriptor descriptor;
	struct memory_fd_metadata metadata;
	VkPhysicalDeviceIDProperties identity;
	VkResult status;

	/* Invalid requests never expose an fd owned by another object or device. */
	if (pFd == NULL)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Clear output ownership before validating the requested device and fd representation. */
	*pFd = -1;
	owner = vulkan_device(device);
	if (owner == NULL ||
	    pGetFdInfo == NULL ||
	    pGetFdInfo->sType != VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR ||
	    pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Descriptor export requires the extension enabled on this logical device. */
	if (!(owner->enabled_extensions & VULKAN_DEVICE_EXTERNAL_MEMORY_FD))
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Only this device's explicitly exportable memory can grant an external reference. */
	memory = vulkan_memory(pGetFdInfo->memory);
	if (memory == NULL ||
	    memory->object.parent != &owner->object ||
	    !(memory->export_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The immutable envelope carries native allocation compatibility, never a renderer-local handle. */
	memset(&identity, 0, sizeof(identity));
	status = vulkan_physical_identity(owner->physical, &identity);
	if (status != VK_SUCCESS)
		return status;

	/* Preserve original public bounds and the native compatibility pair in the immutable payload. */
	memset(&metadata, 0, sizeof(metadata));
	metadata.public_bytes = memory->bytes;
	metadata.memory_type = memory->type_index;
	memcpy(metadata.device_uuid, identity.deviceUUID, VK_UUID_SIZE);
	memcpy(metadata.driver_uuid, identity.driverUUID, VK_UUID_SIZE);

	/* The schema separates this Vulkan envelope from image-only and unrelated kernel handles. */
	memset(&descriptor, 0, sizeof(descriptor));
	descriptor.version = GPU_ABI_VERSION;
	descriptor.size = sizeof(descriptor);
	descriptor.schema = VULKAN_MEMORY_FD_SCHEMA;
	descriptor.metadata_bytes = sizeof(metadata);
	memcpy(descriptor.metadata, &metadata, sizeof(metadata));

	/* Install an fd only after the complete immutable capability has been accepted. */
	status = vulkan_memory_allocation_fd(owner, pGetFdInfo->memory, &descriptor, pFd);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: dup, SCM_RIGHTS, close and process exit preserve ordinary reference semantics. */
	return VK_SUCCESS;
}

/* OPAQUE_FD imports carry their original type; the properties query excludes that handle type. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetMemoryFdPropertiesKHR(
	VkDevice device,
	VkExternalMemoryHandleTypeFlagBits handleType,
	int fd,
	VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
	/* No Linux dma-buf or other queryable foreign fd type is advertised by this implementation. */
	(void)device;
	(void)handleType;
	(void)fd;
	if (pMemoryFdProperties != NULL)
		pMemoryFdProperties->memoryTypeBits = 0U;

	/* Failure: OPAQUE_FD is invalid for this query by the standard's valid-usage contract. */
	return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

/*
 * Allocates either ordinary core memory or an explicitly exportable WSI image.
 */
VkResult
vulkan_memory_allocate(
	VkDevice device,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkBool32 shared,
	VkDeviceMemory *memory)
{
	VkResult error;

	/* Ordinary allocation has no resource identity from another context. */
	error = memory_allocate(device, info, allocator, shared, 0U, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: native storage and any requested WSI export belong to this allocation. */
	return VK_SUCCESS;
}

/*
 * Allocates a shared image while consuming explicit physical conditions synchronously during export.
 * The borrowed condition pointer is never stored in the published allocation or a worker job.
 */
VkResult
vulkan_memory_allocate_placed(
	VkDevice device,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	const struct gpu_placement *placement,
	VkDeviceMemory *memory)
{
	VkResult error;

	/* Native creation and placed export share the existing all-or-nothing allocation lifetime. */
	error = memory_allocate(device, info, allocator, VK_TRUE, 0U, placement, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every specified physical condition was established before handle publication. */
	return VK_SUCCESS;
}

/*
 * Imports a context-attached renderer allocation and consumes its K alias only on success.
 */
VkResult
vulkan_memory_import(
	struct VkDevice_T *device,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	uint32_t resource,
	uint64_t alias,
	VkDeviceMemory *memory)
{
	struct vulkan_memory *storage;
	struct memory_allocation *allocation;
	VkResult error;

	/* The existing resource is independently reference-counted by K and the renderer. */
	error = memory_allocate((VkDevice)device, info, allocator, VK_FALSE, resource, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* The published allocation now owns the imported K alias until vkFreeMemory. */
	storage = vulkan_memory(*memory);
	allocation = storage->backing_private;
	allocation->blob = alias;
	allocation->view_bytes = (size_t)info->allocationSize;

	/* Succeeded: the caller may retire the input capability without losing this allocation. */
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

	/* Shared allocations obtain a mapping only when the application explicitly requests one. */
	if (allocation->token == 0U) {
		status = memory_mapping_token(owner, allocation);
		if (status != VK_SUCCESS)
			return status;
	}

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

/*
 * Exports an existing WSI allocation as a typed fd with immutable image metadata.
 */
VkResult
vulkan_memory_image_fd(
	struct VkDevice_T *device,
	VkDeviceMemory handle,
	struct gpu_image_descriptor *image,
	int *fd)
{
	struct vulkan_memory *memory;
	struct memory_allocation *allocation;
	struct gpu_resource_export request;
	int status;

	/* Only this device's live allocation may supply a capability. */
	*fd = -1;
	memory = vulkan_memory(handle);
	if (memory == NULL || memory->object.parent != &device->object)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Ordinary device-only allocations have no renderer resource to export. */
	allocation = memory->backing_private;
	if (allocation->blob == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* K records the descriptor and installs a separately owned close-on-exec fd. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = allocation->blob;
	request.flags = GPU_HANDLE_CLOEXEC;
	request.fd = -1;
	request.image = *image;
	request.image.allocation_bytes = allocation->view_bytes;
	request.image.memory_type = memory->type_index;

	/* Publish the capability within the same serialized session as the owning resource. */
	vulkan_context_lock(device->object.context);

	status = ioctl(device->object.context->fd, GPU_RESOURCE_EXPORT, &request);

	vulkan_context_unlock(device->object.context);

	/* Failed export leaves no caller-owned capability to release. */
	if (status != 0)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The caller owns this capability independently of the producer's Vulkan object. */
	*image = request.image;
	*fd = request.fd;

	/* Succeeded: immutable metadata describes the allocation held by the new fd. */
	return VK_SUCCESS;
}

/*
 * Exports allocation ownership with immutable metadata supplied by a userspace protocol.
 */
VkResult
vulkan_memory_allocation_fd(
	struct VkDevice_T *device,
	VkDeviceMemory handle,
	struct gpu_allocation_descriptor *description,
	int *fd)
{
	struct vulkan_memory *memory;
	struct memory_allocation *allocation;
	struct gpu_allocation_export request;
	int status;

	/* A failed request publishes neither a capability nor a foreign allocation. */
	*fd = -1;
	memory = vulkan_memory(handle);
	if (memory == NULL || memory->object.parent != &device->object)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A display-only sharing backend must not receive an allocation-only request. */
	if ((device->object.context->capabilities & GPU_CAP_ALLOCATION_SHARE) == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Only a resource-backed allocation has transferable ownership. */
	allocation = memory->backing_private;
	if (allocation->blob == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The memory object supplies allocation bounds; protocol metadata stays byte-for-byte intact. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = allocation->blob;
	request.flags = GPU_HANDLE_CLOEXEC;
	request.fd = -1;
	request.allocation = *description;
	request.allocation.allocation_bytes = allocation->view_bytes;

	/* Export shares the existing resource without exposing the renderer context or memory pointer. */
	vulkan_context_lock(device->object.context);

	status = ioctl(device->object.context->fd, GPU_ALLOCATION_EXPORT, &request);

	vulkan_context_unlock(device->object.context);

	/* No fd ownership escapes a failed reservation or metadata validation. */
	if (status != 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The successful output includes K's device identity and the newly owned capability. */
	*description = request.allocation;
	*fd = request.fd;

	/* Succeeded: the fd retains the complete allocation independently of this Vulkan memory. */
	return VK_SUCCESS;
}

/* Imports the retained allocation after checking immutable API compatibility metadata. */
static VkResult
memory_import_fd(
	struct VkDevice_T *device,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	int fd,
	VkDeviceMemory *memory)
{
	struct gpu_allocation_import request;
	struct memory_fd_metadata metadata;
	struct memory_allocation *allocation;
	struct vulkan_memory *storage;
	VkPhysicalDeviceIDProperties identity;
	VkMemoryAllocateInfo native_info;
	VkResult status;
	VkResult cleanup;
	int error;
	int compatible;

	/* Resolve the receiver's actual device/driver before attaching any foreign resource. */
	if (!(device->object.context->capabilities & GPU_CAP_ALLOCATION_SHARE))
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* A failed native identity query must not attach any destination alias. */
	memset(&identity, 0, sizeof(identity));
	status = vulkan_physical_identity(device->physical, &identity);
	if (status != VK_SUCCESS)
		return status;

	/* Kernel import borrows the fd; failures still leave descriptor ownership with the caller. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.fd = fd;

	/* The context retains the imported alias while the library validates its metadata. */
	vulkan_context_lock(device->object.context);

	error = ioctl(device->object.context->fd, GPU_ALLOCATION_IMPORT, &request);
	if (error != 0)
		error = errno;

	vulkan_context_unlock(device->object.context);

	/*
	 * An image capability (what the Wayland WSI sends) has no allocation
	 * envelope; it is imported as the image's whole allocation instead.
	 */
	if (error == EINVAL) {
		status = memory_import_image_fd(device, info, allocator, fd, memory);
		return status;
	}

	/* Rejection has not transferred the caller's descriptor ownership. */
	if (error != 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The library schema is separate from linear scanout and arbitrary kernel allocation metadata. */
	status = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	memset(&metadata, 0, sizeof(metadata));
	if (request.allocation.schema == VULKAN_MEMORY_FD_SCHEMA && request.allocation.metadata_bytes == sizeof(metadata)) {
		memcpy(&metadata, request.allocation.metadata, sizeof(metadata));
		compatible = memcmp(metadata.device_uuid, identity.deviceUUID, VK_UUID_SIZE);
		compatible |= memcmp(metadata.driver_uuid, identity.driverUUID, VK_UUID_SIZE);
		if (compatible == 0 &&
		    metadata.reserved == 0U &&
		    metadata.public_bytes == info->allocationSize &&
		    metadata.memory_type == info->memoryTypeIndex &&
		    request.allocation.allocation_bytes >= metadata.public_bytes &&
		    request.allocation.allocation_bytes <= SIZE_MAX &&
		    request.resource_id != 0U)
			status = VK_SUCCESS;
	}

	/* Native import establishes a distinct VkDeviceMemory referencing the same physical allocation. */
	if (status == VK_SUCCESS) {
		/* OPAQUE native imports require the original page-rounded allocation extent. */
		native_info = *info;
		native_info.allocationSize = request.allocation.allocation_bytes;
		status = vulkan_memory_import(
			device,
			&native_info,
			allocator,
			request.resource_id,
			request.handle,
			memory);
	}

	/* Failed metadata or native allocation leaves only the unpublished K alias to retire. */
	if (status != VK_SUCCESS) {
		vulkan_context_lock(device->object.context);

		cleanup = vulkan_resource_destroy(device->object.context, request.handle);

		vulkan_context_unlock(device->object.context);

		if (cleanup != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;

		/* The original descriptor remains caller-owned after import failure. */
		return status;
	}

	/* CPU mapping uses native page-rounded bounds while public API access retains original bounds. */
	storage = vulkan_memory(*memory);
	storage->bytes = info->allocationSize;
	allocation = storage->backing_private;
	allocation->view_bytes = (size_t)request.allocation.allocation_bytes;
	error = close(fd);
	(void)error;

	/* Succeeded: the imported memory owns its reference and the input fd has been consumed. */
	return VK_SUCCESS;
}

/*
 * Imports the allocation of an exported image capability as device memory.
 * The kernel checks the device and returns the image's description; the
 * requested memory type must be the image's and the requested size must fit
 * its allocation.  The caller binds its own image of the described layout.
 */
static VkResult
memory_import_image_fd(
	struct VkDevice_T *device,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	int fd,
	VkDeviceMemory *memory)
{
	struct gpu_resource_import request;
	struct memory_allocation *allocation;
	struct vulkan_memory *storage;
	VkMemoryAllocateInfo native_info;
	VkResult status;
	VkResult cleanup;
	int error;

	/* Image capabilities travel only between contexts that share images. */
	if (!(device->object.context->capabilities & GPU_CAP_SHARE))
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The fd is the only input; the kernel supplies the image's metadata. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.fd = fd;

	/* Attaches an alias of the image's allocation to this context. */
	vulkan_context_lock(device->object.context);

	error = ioctl(device->object.context->fd, GPU_RESOURCE_IMPORT, &request);

	vulkan_context_unlock(device->object.context);

	/* Rejection has not transferred the caller's descriptor ownership. */
	if (error != 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The request must name the image's memory type and fit its allocation. */
	status = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (request.image.memory_type == info->memoryTypeIndex &&
	    info->allocationSize != 0U &&
	    info->allocationSize <= request.image.allocation_bytes &&
	    request.image.allocation_bytes <= SIZE_MAX &&
	    request.resource_id != 0U)
		status = VK_SUCCESS;

	/* The memory references the whole native allocation, as an OPAQUE import does. */
	if (status == VK_SUCCESS) {
		native_info = *info;
		native_info.allocationSize = request.image.allocation_bytes;
		status = vulkan_memory_import(
			device,
			&native_info,
			allocator,
			request.resource_id,
			request.handle,
			memory);
	}

	/* A failed import leaves only the unpublished alias to retire. */
	if (status != VK_SUCCESS) {
		vulkan_context_lock(device->object.context);

		cleanup = vulkan_resource_destroy(device->object.context, request.handle);

		vulkan_context_unlock(device->object.context);

		if (cleanup != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;

		/* The original descriptor remains caller-owned after import failure. */
		return status;
	}

	/* Public bounds are the requested size; mapping uses the whole allocation. */
	storage = vulkan_memory(*memory);
	storage->bytes = info->allocationSize;
	allocation = storage->backing_private;
	allocation->view_bytes = (size_t)request.image.allocation_bytes;
	error = close(fd);
	(void)error;

	/* Succeeded: the imported memory owns its reference and the fd is consumed. */
	return VK_SUCCESS;
}

/* Obtains a process-local mmap token only for an explicitly requested coherent CPU view. */
static VkResult
memory_mapping_token(
	struct VkDevice_T *device,
	struct memory_allocation *allocation)
{
	struct gpu_resource_map request;
	int error;

	/* A token is never inferred from a native allocation or a foreign process's address. */
	if (allocation->blob == 0U || allocation->view_bytes == 0U)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* Resolve the imported allocation into this process's mapping namespace. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = allocation->blob;

	/* Resolve one mapping token while preserving this context's request serialization. */
	vulkan_context_lock(device->object.context);

	error = ioctl(device->object.context->fd, GPU_RESOURCE_MAP, &request);

	vulkan_context_unlock(device->object.context);

	/* A backend without a coherent view cannot satisfy an explicit CPU mapping. */
	if (error != 0)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* The kernel must describe the complete retained allocation with a page-aligned token. */
	if (request.offset == 0U ||
	    request.offset > INT64_MAX ||
	    request.bytes != allocation->view_bytes ||
	    (request.offset & (VULKAN_MEMORY_PAGE - 1U)) != 0U)
		return VK_ERROR_MEMORY_MAP_FAILED;

	/* Cache only the fully validated process-local token. */
	allocation->token = request.offset;

	/* Succeeded: later mappings reuse this token without changing allocation ownership. */
	return VK_SUCCESS;
}

/* Creates local memory ownership around native allocation, export, or resource import. */
static VkResult
memory_allocate(
	VkDevice device,
	const VkMemoryAllocateInfo *pAllocateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkBool32 shared,
	uint32_t imported,
	const struct gpu_placement *placement,
	VkDeviceMemory *pMemory)
{
	struct VkDevice_T *owner;
	struct memory_allocation *allocation;
	struct vulkan_memory *memory;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	const VkBaseInStructure *next;
	const VkExportMemoryAllocateInfo *export;
	VkMemoryPropertyFlags properties;
	uint64_t native_bytes;
	uint64_t extension_present;
	uint64_t present;
	uint64_t identifier;
	VkResult status;
	VkResult cleanup;
	uint32_t external_type;

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

	/* Private scanout retains DMA_BUF; public OPAQUE follows the negotiated renderer profile. */
	external_type = VULKAN_EXTERNAL_MEMORY_DMABUF;
	for (next = pAllocateInfo->pNext; next != NULL; next = next->pNext) {
		/* Other allocation extensions do not choose an export representation. */
		if (next->sType != VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
			continue;

		/* Only the explicit public reference-bearing handle requests the opaque native route. */
		export = (const VkExportMemoryAllocateInfo *)next;
		if (export->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
		    owner->object.context->external_memory_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
			external_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	}

	/* CPU-visible and shared allocations reserve whole pages without changing public bounds. */
	properties = owner->physical->memory.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
	native_bytes = pAllocateInfo->allocationSize;
	if (shared != VK_FALSE || (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
		/* Noncoherent types are filtered from the advertised host-visible set. */
		if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
		    (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
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
	allocation->external_type = external_type;

	/* Reserve the renderer identity before emitting any creation request. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Leave without publishing an allocation the renderer did not accept. */
		return status;
	}

	/* Export or import adds one native extension while ordinary allocations keep an empty chain. */
	extension_present = 0U;
	if (shared != VK_FALSE) {
		/* Export declares a renderer-native allocation shareable with another context. */
		extension_present = 1U;
	} else if (imported != 0U) {
		/* Import identifies storage already attached to this independent context. */
		extension_present = 1U;
	}

	/* Only protocol values cross the wire; application allocator pointers stay local. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkAllocateMemory);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_pointer(&writer, pAllocateInfo);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	vulkan_write_u64(&writer, extension_present);

	/* The resource import takes precedence over allocation export when a resource is supplied. */
	if (imported != 0U) {
		/* The renderer imports a resource already attached to this independent context. */
		vulkan_write_u32(&writer, 1000384002U);
		vulkan_write_u64(&writer, 0U);
		vulkan_write_u32(&writer, imported);
	} else if (shared != VK_FALSE) {
		/* This renderer-only export type is not a guest dma-buf interface. */
		vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
		vulkan_write_u64(&writer, 0U);
		vulkan_write_u32(&writer, external_type);
	}

	/* Complete the allocation using renderer values and a process-local output identity. */
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
	if (imported == 0U &&
	    (shared != VK_FALSE ||
	     (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)) {
		/* Prepare the persistent export before any application can resolve the handle. */
		status = memory_export(owner, allocation, native_bytes, shared, placement);
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

/* Exports one renderer allocation while serializing all native operations on its fd. */
static VkResult
memory_export(
	struct VkDevice_T *device,
	struct memory_allocation *allocation,
	uint64_t bytes,
	VkBool32 shared,
	const struct gpu_placement *placement)
{
	struct vulkan_context *context;
	struct gpu_blob_create_placed placed;
	struct gpu_resource_map request;
	uint32_t flags;
	uint32_t resource;
	VkResult status;
	VkResult cleanup;
	int error;

	/* A physical device must not advertise host visibility without shared mapping. */
	context = device->object.context;
	if (shared == VK_FALSE && (context->capabilities & GPU_CAP_MAPPING) == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* CPU mappings need an aperture; WSI sharing instead enables independent context import. */
	flags = GPU_BLOB_MAPPABLE;
	if (shared != VK_FALSE) {
		flags = GPU_BLOB_SHAREABLE;
		if (allocation->external_type == VULKAN_EXTERNAL_MEMORY_DMABUF)
			flags |= GPU_BLOB_CROSS_DEVICE;

		/* Host-visible allocations allow later explicit mapping without mapping them during export. */
		if (allocation->memory.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
			flags |= GPU_BLOB_MAPPABLE;
	}

	/* Create the persistent export and any mmap token as one serialized session transaction. */
	vulkan_context_lock(context);

	if (placement != NULL) {
		/* This private WSI request reaches K before the exported image can acquire any users. */
		memset(&placed, 0, sizeof(placed));
		placed.blob.version = GPU_ABI_VERSION;
		placed.blob.size = sizeof(placed);
		placed.blob.bytes = bytes;
		placed.blob.blob_id = allocation->memory.object.wire_id;
		placed.blob.flags = flags;
		placed.placement = *placement;
		error = ioctl(context->fd, GPU_BLOB_CREATE_PLACED, &placed);
		if (error != 0) {
			/* Unsupported physical backing permits one explicit WSI fallback; allocation failure does not. */
			if (errno == ENOTSUP || errno == ENOTTY) {
				status = VK_ERROR_FEATURE_NOT_PRESENT;
			} else if (errno == ENOMEM) {
				status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			} else {
				/* Uncertain backend ownership must retire with its context instead of native-free retry. */
				memory_lost(device);
				status = VK_ERROR_DEVICE_LOST;
			}
		} else if (placed.blob.handle == 0U || placed.blob.resource_id == 0U) {
			/* A malformed success is terminal; inventing an unowned native identity is forbidden. */
			memory_lost(device);
			status = VK_ERROR_DEVICE_LOST;
		} else {
			/* Publication transfers the placed blob into the ordinary allocation cleanup path. */
			allocation->blob = placed.blob.handle;
			resource = placed.blob.resource_id;
			status = VK_SUCCESS;
		}
	} else {
		/* Public memory and window-system paths retain their unchanged unconstrained export. */
		status = vulkan_resource_blob_flags(
			context,
			bytes,
			allocation->memory.object.wire_id,
			flags,
			&allocation->blob,
			&resource);
	}

	/* Failed export is unwound by the caller before local or native memory becomes public. */
	if (status != VK_SUCCESS) {
		vulkan_context_unlock(context);
		return status;
	}

	/* GPU-only exports never allocate a user MMIO mapping or read pixels through it. */
	if (shared != VK_FALSE) {
		allocation->view_bytes = (size_t)bytes;
		vulkan_context_unlock(context);

		/* Succeeded: shared GPU storage remains allocation-owned without a CPU view. */
		return VK_SUCCESS;
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
	vulkan_device_error(device, VK_ERROR_DEVICE_LOST);
	vulkan_context_error(device->object.context, VK_ERROR_DEVICE_LOST);

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
