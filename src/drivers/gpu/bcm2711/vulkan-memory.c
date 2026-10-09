/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native allocation declarations precede physical placement; independent views keep BLOB and GPU owners alive. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-memory.h"

/* The physical query exposes one bounded logical heap and one coherent memory type. */
#define VULKAN_MEMORY_BUDGET (256ULL << 20)
#define VULKAN_MEMORY_IMPORT 1000384002U
#define VULKAN_MEMORY_OPAQUE 1U
#define VULKAN_MEMORY_NATIVE_SHARE 0x200U

static int allocate_memory(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int free_memory(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader);
static int release_memory(struct bcm2711_vulkan_session *session, void *payload);
static int placement_requirements(const struct gpu_placement *placement, uint64_t *limit, size_t *alignment);
static int backing_matches(struct bcm2711_buffer *buffer, uint64_t bytes, uint64_t limit, size_t alignment);
static void allocation_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Executes allocation and retirement commands without transferring ownership of the real client's BLOB aliases.
 */
int
bcm2711_vulkan_memory_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Other typed routers retain responsibility for their own object kinds. */
	*handled = 0;
	if (opcode != GPU_OP_ALLOCATE_MEMORY && opcode != GPU_OP_FREE_MEMORY)
		return 0;
	*handled = 1;

	/* Ordinary void commands still request an echoed opcode; allocations require their result body. */
	if (requested > 1 || (opcode == GPU_OP_ALLOCATE_MEMORY && requested != 1))
		return EINVAL;

	/* The controller mutex serializes both the aggregate declaration budget and independent view ownership. */
	if (opcode == GPU_OP_ALLOCATE_MEMORY)
		error = allocate_memory(session, reader, reply);
	else
		error = free_memory(session, reader);
	if (error != 0)
		return error;

	/* Succeeded: one native allocation declaration or identity retired. */
	return 0;
}

/*
 * Acquires one coherent allocation reference after validating the first BLOB's actual physical placement conditions.
 */
int
bcm2711_vulkan_memory_blob(
	struct bcm2711_vulkan_session *session,
	const struct gpu_blob_create *request,
	const struct gpu_placement *placement,
	struct bcm2711_buffer **buffer)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_buffer *created;
	struct bcm2711_v3d_view *view;
	uint64_t limit;
	size_t alignment;
	int error;

	/* An unbound or closing protocol namespace grants no renderer allocation identity. */
	*buffer = NULL;
	if (session == NULL)
		return ENOTSUP;
	if (session->closing || request->blob_id == 0)
		return EINVAL;

	/* CPU mapping, sharing and independent-device permission retain their actual common-core meanings. */
	if ((request->flags & ~(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0)
		return EINVAL;
	if ((request->flags & GPU_BLOB_CROSS_DEVICE) != 0 &&
	    (request->flags & GPU_BLOB_SHAREABLE) == 0)
		return EINVAL;

	/* Only this open's live typed memory identity can select a declaration. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, request->blob_id);
	if (object == NULL)
		return EINVAL;
	memory = object->payload;
	if (request->bytes != memory->bytes)
		return EINVAL;
	if ((request->flags & GPU_BLOB_SHAREABLE) != 0 && memory->external_type == 0)
		return EINVAL;

	/* A native share marker is a protocol hint; it does not promise a Linux dma-buf descriptor. */
	error = placement_requirements(placement, &limit, &alignment);
	if (error != 0)
		return error;

	/* The first export's immutable conditions must precede RAM allocation and native GPU publication. */
	if (memory->view == NULL) {
		error = bcm2711_buffer_create_uncached(memory->bytes, limit, alignment, &created);
		if (error != 0)
			return error;

		/* Failed native flush owns its quarantine independently; the unpublished source reference can retire. */
		error = bcm2711_v3d_memory_map(&session->render->device->space, created, &view);
		bcm2711_buffer_release(created);
		if (error != 0)
			return error;
		memory->view = view;
	}

	/* Repeated aliases must satisfy their conditions from actual existing storage, without relocation. */
	if (memory->view->quarantined)
		return EIO;
	created = memory->view->buffer;
	error = backing_matches(created, memory->bytes, limit, alignment);
	if (error != 0)
		return error;

	/* The new resource constructor receives its own reference; memory keeps its independently owned GPU view. */
	bcm2711_buffer_retain(created);
	*buffer = created;

	/* Succeeded: every accepted placement and HOST_COHERENT condition follows from the actual native backing. */
	return 0;
}

/* Decodes the real client's exact allocation chain and reserves storage only after the declared device is retained. */
static int
allocate_memory(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_resource *resource;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_memory *memory;
	uint64_t device_id;
	uint64_t present;
	uint64_t chain;
	uint64_t next;
	uint64_t bytes;
	uint64_t rounded;
	uint64_t allocator;
	uint64_t identity;
	uint32_t type;
	uint32_t extension;
	uint32_t argument;
	uint32_t memory_type;
	int error;
	int retired;

	/* The supported wire uses one optional import/export record, never an application allocator pointer. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1 ||
	    type != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO || chain > 1)
		return EINVAL;
	extension = 0;
	argument = 0;
	if (chain != 0) {
		extension = drv_i915_wire_read_u32(reader);
		next = drv_i915_wire_read_u64(reader);
		argument = drv_i915_wire_read_u32(reader);
		if (reader->error != 0 || next != 0)
			return EINVAL;
		if (extension != VULKAN_MEMORY_IMPORT &&
		    extension != VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
			return ENOTSUP;
	}

	/* Decode the full declaration before acquiring any physical or logical ownership. */
	bytes = drv_i915_wire_read_u64(reader);
	memory_type = drv_i915_wire_read_u32(reader);
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0 || present != 1 || identity == 0)
		return EINVAL;
	if (bytes == 0 || bytes > VULKAN_MEMORY_BUDGET || memory_type != 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, identity);
	if (object != NULL)
		return EEXIST;

	/* Opaque and private WSI sharing hints both use native typed allocation capabilities. */
	if (extension == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO &&
	    argument != VULKAN_MEMORY_OPAQUE && argument != VULKAN_MEMORY_NATIVE_SHARE)
		return ENOTSUP;
	resource = NULL;
	if (extension == VULKAN_MEMORY_IMPORT) {
		resource = bcm2711_render_find(session->render, argument);
		if (resource == NULL || !resource->blob || resource->view->quarantined ||
		    !resource->view->buffer->uncached || bytes > resource->view->buffer->bytes)
			return EINVAL;
	}

	/* The queried heap budget bounds aggregate live declarations, including conservative charges for imported aliases. */
	controller = session->render->device;
	rounded = (bytes + 4095U) & ~4095ULL;
	if (controller->vulkan_memory_bytes > VULKAN_MEMORY_BUDGET ||
	    rounded > VULKAN_MEMORY_BUDGET - controller->vulkan_memory_bytes) {
		allocation_reply(reply, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
		return 0;
	}

	/* An ordinary allocation has no physical storage until its first placed BLOB arrives. */
	memory = kern_calloc(1, sizeof(*memory));
	if (memory == NULL) {
		allocation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* Each declaration holds its actual device independently of the device's published identity. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(memory);
		return error;
	}

	/* Complete the retained declaration before assigning any imported native view. */
	memory->device = device;
	memory->bytes = bytes;
	memory->charged = rounded;
	if (extension == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
		memory->external_type = argument;

	/* Imported native storage remains coherent and retains its GPU mapping independently of the original resource. */
	if (resource != NULL) {
		bcm2711_v3d_memory_retain(resource->view);
		memory->view = resource->view;
		if (resource->shareable)
			memory->external_type = VULKAN_MEMORY_OPAQUE;
	}

	/* Charge the complete declaration before its registry entry can become visible. */
	controller->vulkan_memory_bytes += rounded;

	/* A failed identity publication unwinds every acquired dependency and charge through the same destructor. */
	error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_MEMORY, identity, memory, release_memory, &object);
	if (error != 0) {
		/* Host namespace exhaustion has a structured Vulkan result; native retirement errors take precedence. */
		if (error == ENOMEM || error == ENOSPC) {
			error = release_memory(session, memory);
			if (error != 0)
				return error;
			allocation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Other framing/publication refusals still release all unpublished ownership. */
		retired = release_memory(session, memory);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Acknowledge exactly the fully published declaration identity. */
	allocation_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: a typed memory declaration owns its logical budget and optional imported physical view. */
	return 0;
}

/* Withdraws one memory identity while resource, bound-object and prepared-job references remain independent. */
static int
free_memory(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_memory *memory;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* A void command still has its transport opcode echo, but no parameter reply. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	if (identity == 0)
		return 0;

	/* Parent equality prevents an allocation of another logical device from retiring. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, identity);
	if (object == NULL)
		return EINVAL;
	memory = object->payload;
	if (memory->device != device)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_MEMORY, identity);
	if (error != 0)
		return error;

	/* Succeeded: only this declaration's registry reference has retired. */
	return 0;
}

/* Retires the declaration while failed GPU unmapping keeps physical storage in the independent VA quarantine owner. */
static int
release_memory(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_render_device *controller;
	int error;
	int parent_error;

	/* GPU retirement precedes logical budget and host metadata retirement. */
	memory = payload;
	controller = session->render->device;
	error = 0;
	if (memory->view != NULL)
		error = bcm2711_v3d_memory_release(&controller->space, memory->view);

	/* Charges count live VkMemory declarations, not physical measurements of surviving resource/VM aliases. */
	if (controller->vulkan_memory_bytes < memory->charged)
		__builtin_trap();
	controller->vulkan_memory_bytes -= memory->charged;
	parent_error = bcm2711_vulkan_object_release(memory->device);
	kern_free(memory);

	/* Preserve a native storage retirement error ahead of a dependent root retirement error. */
	if (error != 0)
		return error;
	if (parent_error != 0)
		return parent_error;

	/* Succeeded: no declared allocation or dependency remains owned by this payload. */
	return 0;
}

/* Resolves accepted immutable placement requirements without assuming cached nonsnooping RAM is coherent. */
static int
placement_requirements(
	const struct gpu_placement *placement,
	uint64_t *limit,
	size_t *alignment)
{
	/* All native allocations are contiguous Normal uncached RAM below the display's one-GiB reachability boundary. */
	*limit = 0x3fffffffU;
	*alignment = 4096;
	if (placement == NULL)
		return 0;
	if (placement->reserved != 0 ||
	    (placement->flags & ~(GPU_PLACEMENT_DMA32 | GPU_PLACEMENT_CONTIGUOUS | GPU_PLACEMENT_COHERENT)) != 0)
		return EINVAL;

	/* Every accepted condition is enforced by creation or checked from existing physical storage. */
	if (placement->max_dma_address != 0 && placement->max_dma_address < *limit)
		*limit = placement->max_dma_address;
	if (placement->alignment > 0x40000000ULL)
		return ENOTSUP;
	if (placement->alignment != 0 &&
	    (placement->alignment & (placement->alignment - 1U)) != 0)
		return EINVAL;
	if (placement->alignment > *alignment)
		*alignment = (size_t)placement->alignment;

	/* Succeeded: the physical allocator can establish each represented condition before publication. */
	return 0;
}

/* Checks a repeat export against the actual immutable uncached run instead of silently migrating live aliases. */
static int
backing_matches(
	struct bcm2711_buffer *buffer,
	uint64_t bytes,
	uint64_t limit,
	size_t alignment)
{
	uint64_t rounded;

	/* The coherent allocation descriptor must describe a complete contiguous page-rounded run. */
	rounded = (bytes + 4095U) & ~4095ULL;
	if (!buffer->uncached || buffer->address == NULL ||
	    buffer->bytes < bytes || buffer->memory.size < rounded)
		return ENOTSUP;

	/* The inclusive device-address ceiling covers the whole actual allocation, not only the requested prefix. */
	if ((buffer->memory.paddr & (alignment - 1U)) != 0 ||
	    buffer->memory.paddr > limit || buffer->memory.size - 1U > limit - buffer->memory.paddr)
		return ENOTSUP;

	/* Succeeded: all existing aliases retain the same physical and CPU mapping policy. */
	return 0;
}

/* Writes the ordinary allocation result and exactly one echoed guest-chosen typed identity. */
static void
allocation_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* The output pointer is present even on failure, with a null identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);
}
