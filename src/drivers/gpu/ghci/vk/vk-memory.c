/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Memory, buffers and images of the Vulkan executor, and the executor
 * heap of each device.
 *
 * A VkDeviceMemory is host memory below the GPU's limit (or the memory of
 * a resource the node session imported), held by the object and mapped
 * once into the device's GHCI session; its CPU view is the blob libvulkan
 * makes of it.  On a discrete GPU, memory of the device-local type is the
 * GPU's local memory instead, made for export or not (an image another
 * process shows lives in local memory too), which the GPU clears before
 * the allocation returns (it may hold what an earlier client left); the
 * processor never reads or writes it, and memory of the host-visible
 * type stays in host memory.  Imported memory is the resource's,
 * wherever that lies.  A buffer or an image is a range of one memory
 * object once it is bound.  Every image is laid out in linear rows
 * (section 8.5.2 of the specification): attachments and storage images
 * must be, and the others may be.
 *
 * The executor heaps are what the driver itself writes for the GPU in a
 * device's session: blocks of host memory mapped once and handed out in
 * pieces, a block twice the last when none has room.  The code heap
 * (pipeline state records and programs) is mapped executable, the data
 * heap (scratch, events, query pools, a submission's temporary memory)
 * is not.  A run that work in the ring may still read goes back once
 * that work's ticket has passed.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * How long one wait for the GPU's clear of new local memory sleeps before
 * it looks again.  The allocation waits on until the clear's ticket
 * passes or a reset or the loss of the device ends it; a busy GPU only
 * makes it longer.
 */
#define CLEAR_WAIT_MS			1000U

/* A buffer's size in memory is a whole number of words. */
#define BUFFER_GRANULE			4U

static int allocate_memory(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t memory_back(struct ghci_vk_session *session, struct ghci_vk_object *object, uint32_t imported, int local, uint64_t bytes);
static uint32_t local_clear(struct ghci_vk_session *session, struct ghci_vk_object *object);
static uint32_t memory_types(struct ghci_vk_session *session);
static int free_memory(struct ghci_vk_session *session, enum ghci_vk_kind kind, struct i915_wire_reader *reader);
static int commitment(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_buffer(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_image(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t image_admit(struct ghci_vk_session *session, const VkImageCreateInfo *info, struct ghci_vk_image *image);
static int requirements(struct ghci_vk_session *session, enum ghci_vk_kind kind, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int bind(struct ghci_vk_session *session, enum ghci_vk_kind kind, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int subresource_layout(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int sparse_requirements(uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static struct ghci_vk_device *device_of(struct ghci_vk_session *session, uint64_t handle);
static uint64_t round_up(uint64_t value, uint64_t granule);
static int heap_grow(struct ghci_vk_session *session, struct ghci_vk_device *device, enum ghci_vk_heap heap, uint32_t least);

/*
 * Runs a memory, buffer or image command; *handled stays clear for
 * another group's number.
 */
int
ghci_vk_memory_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_ALLOCATE_MEMORY:
		error = allocate_memory(session, requested, reader, reply);
		break;
	case GPU_OP_FREE_MEMORY:
		error = free_memory(session, GHCI_VK_MEMORY, reader);
		break;
	case GPU_OP_GET_DEVICE_MEMORY_COMMITMENT:
		error = commitment(session, requested, reader, reply);
		break;
	case GPU_OP_CREATE_BUFFER:
		error = create_buffer(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER:
		error = free_memory(session, GHCI_VK_BUFFER, reader);
		break;
	case GPU_OP_CREATE_IMAGE:
		error = create_image(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE:
		error = free_memory(session, GHCI_VK_IMAGE, reader);
		break;
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
		error = requirements(session, GHCI_VK_BUFFER, requested, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
		error = requirements(session, GHCI_VK_IMAGE, requested, reader, reply);
		break;
	case GPU_OP_BIND_BUFFER_MEMORY:
		error = bind(session, GHCI_VK_BUFFER, requested, reader, reply);
		break;
	case GPU_OP_BIND_IMAGE_MEMORY:
		error = bind(session, GHCI_VK_IMAGE, requested, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT:
		error = subresource_layout(session, requested, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_SPARSE_MEMORY_REQUIREMENTS:
		error = sparse_requirements(requested, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Reports the allocation of one of a node session's VkDeviceMemory
 * objects for its blob; NULL when the session has no such object.  The
 * caller holds the device's lock.
 */
struct ghci_allocation *
ghci_vk_memory_blob(
	struct ghci_device *device,
	struct ghci_session *owner,
	uint64_t blob_id)
{
	struct ghci_vk_object *object;

	(void)device;

	/* The memory object of that identity. */
	if (owner->vk == NULL)
		return NULL;
	object = ghci_vk_object_find(owner->vk, GHCI_VK_MEMORY, blob_id);
	if (object == NULL)
		return NULL;

	/* Succeeded: its allocation, which the blob's resource will hold too. */
	return object->as.memory.allocation;
}

/*
 * Takes bytes of one of a device's executor heaps (code or data),
 * aligned to alignment (a power of two), growing that heap when no block
 * has room.  Runs let go whose tickets have passed go back first.
 * Returns 0 with the block and the offset in it, or the error of the
 * growth (ENOSPC, ENOMEM, EIO).
 */
int
ghci_vk_heap_take(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	enum ghci_vk_heap heap,
	uint32_t bytes,
	uint32_t alignment,
	struct ghci_vk_heap_block **block,
	uint32_t *offset)
{
	struct ghci_vk_heap_block *each;
	int error;

	/* What the GPU has passed is room again. */
	ghci_vk_heap_reclaim(session, device);

	/* The blocks in turn, then a new one. */
	for (;;) {
		for (each = device->heap[heap]; each != NULL; each = each->next) {
			error = ghci_range_alloc(&each->map, bytes, alignment, offset);
			if (error == 0) {
				*block = each;
				return 0;
			}
		}

		/* No block has room: one more, large enough. */
		error = heap_grow(session, device, heap, bytes + alignment);
		if (error != 0)
			return error;
	}
}

/* Gives back bytes of the executor heap that nothing placed in the ring reads. */
void
ghci_vk_heap_give(
	struct ghci_vk_heap_block *block,
	uint32_t offset)
{
	/* The run goes back to its block; nothing else holds it. */
	if (block != NULL)
		(void)ghci_range_free(&block->map, offset);
}

/*
 * Lets a run of the executor heap go once the work placed until now has
 * passed: once ticket has (NULL: the device's last submission), or at
 * once when the device has placed nothing.  A record that cannot be made
 * keeps the run until the device's heap goes.
 */
void
ghci_vk_heap_retire(
	struct ghci_vk_device *device,
	struct ghci_vk_heap_block *block,
	uint32_t offset,
	const struct ghci_ticket *ticket)
{
	struct ghci_vk_retired *retired;
	struct ghci_ticket last;
	unsigned long irq;
	int submitted;

	/* Nothing to let go. */
	if (block == NULL)
		return;

	/* The ticket the run waits for. */
	if (ticket != NULL) {
		last = *ticket;
		submitted = 1;
	} else {
		irq = spin_lock_irqsave(&device->session->device->irq_lock);

		last = device->last;
		submitted = device->submitted;

		spin_unlock_irqrestore(&device->session->device->irq_lock, irq);
	}

	/* Nothing placed reads it, or its ticket has passed. */
	if (!submitted || ghci_cp_ticket(device->session->device, &last) != GHCI_TICKET_PENDING) {
		(void)ghci_range_free(&block->map, offset);
		return;
	}

	/* It waits on the device's list. */
	retired = kern_malloc(sizeof(*retired));
	if (retired == NULL)
		return;
	retired->block = block;
	retired->offset = offset;
	retired->ticket = last;
	retired->next = device->retired;
	device->retired = retired;
}

/* Gives back the runs a device let go whose tickets have passed (or a reset ended). */
void
ghci_vk_heap_reclaim(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device)
{
	struct ghci_vk_retired **position;
	struct ghci_vk_retired *retired;

	/* Each run, in turn. */
	position = &device->retired;
	while (*position != NULL) {
		retired = *position;
		if (ghci_cp_ticket(session->device, &retired->ticket) == GHCI_TICKET_PENDING) {
			position = &retired->next;
			continue;
		}
		*position = retired->next;
		(void)ghci_range_free(&retired->block->map, retired->offset);
		kern_free(retired);
	}
}

/*
 * Lets a device's executor heaps go: each block is unmapped and its
 * memory goes back once the unmap passed (which orders it after the
 * work that read it).  The device's objects are gone.
 */
void
ghci_vk_heap_fini(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device)
{
	struct ghci_vk_heap_block *block;
	struct ghci_vk_retired *retired;
	uint32_t heap;

	/* The records of the runs that waited: their blocks go below. */
	while (device->retired != NULL) {
		retired = device->retired;
		device->retired = retired->next;
		kern_free(retired);
	}

	/* Each block of each heap in turn. */
	for (heap = 0U; heap < GHCI_VK_HEAPS; heap++) {
		while (device->heap[heap] != NULL) {
			block = device->heap[heap];
			device->heap[heap] = block->next;

			/* The GPU address goes with the unmap's fence, and the block lets its memory go. */
			ghci_vm_unmap(session->device, block->mapping);
			mutex_lock(&session->device->lock);

			ghci_memory_unhold(session->device, block->allocation);

			mutex_unlock(&session->device->lock);
			kern_free(block);
		}
		device->heap_bytes[heap] = 0U;
	}
}

/*
 * Lets a VkDeviceMemory go: its mapping is unmapped (the mapping holds
 * the memory until the unmap passed) and the object's own hold goes.
 */
void
ghci_vk_memory_release(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object)
{
	struct ghci_vk_memory *memory;

	/* The mapping, when the memory got one. */
	memory = &object->as.memory;
	if (memory->mapping != NULL)
		ghci_vm_unmap(session->device, memory->mapping);
	memory->mapping = NULL;

	/* The object's hold. */
	if (memory->allocation != NULL) {
		mutex_lock(&session->device->lock);

		ghci_memory_unhold(session->device, memory->allocation);

		mutex_unlock(&session->device->lock);
	}
	memory->allocation = NULL;
}

/* Fills the linear layout of an image (the caller checked that it fits the heap's size). */
void
ghci_vk_image_layout(
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	uint32_t levels,
	uint32_t layers,
	uint32_t samples,
	uint32_t texel_bytes,
	uint32_t block,
	struct ghci_layout_image *layout)
{
	/* The extent, the levels, the layers, the samples and the texel. */
	layout->texel_bytes = texel_bytes;
	layout->block = block;
	layout->width = width;
	layout->height = height;
	layout->depth = depth;
	layout->levels = levels;
	layout->layers = layers;
	layout->samples = samples;
}

/*
 * vkAllocateMemory: [device][u64 1][u32 MEMORY_ALLOCATE_INFO][u64 chain]
 * (a chained [u32 sType][u64 0][u32 value]: an import of a resource of
 * the session, or an export) [u64 size][u32 type][u64 0][u64 1][memory] ->
 * [result][u64 1][memory].
 */
static int
allocate_memory(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t chain;
	uint64_t bytes;
	uint64_t memory;
	uint32_t record;
	uint32_t value;
	uint32_t type;
	uint32_t imported;
	uint32_t result;
	int local;
	int error;

	/* The device, the allocate info's head and its one chained record. */
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	record = 0U;
	value = 0U;
	if (chain != 0U) {
		record = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		value = drv_i915_wire_read_u32(reader);
	}

	/* The size, the type and the tail. */
	bytes = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	memory = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U || chain > 1U)
		return EINVAL;

	/* A device of the session, and the import's resource id. */
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;
	imported = 0U;
	if (chain != 0U && record == GPU_OP_RECORD_IMPORT_RESOURCE)
		imported = value;
	else if (chain != 0U && record != VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
		return EINVAL;

	/* The size and the type the device has. */
	result = (uint32_t)VK_SUCCESS;
	if (type >= memory_types(session) || bytes == 0U)
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	else if (bytes > ghci_vk_caps_heap_bytes(&session->device->caps))
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/*
	 * Local memory for the device-local type of a discrete GPU, made for
	 * export or not (an importer maps it as local memory), unless the
	 * memory is imported (it is the resource's).
	 */
	local = 0;
	if (memory_types(session) == 2U && type == GHCI_VK_TYPE_LOCAL && imported == 0U)
		local = 1;

	/* The object, then its memory and its mapping; a failure takes the object back. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_MEMORY, memory, device, &object);
		if (error != 0)
			return error;
		object->as.memory.type = type;
		result = memory_back(session, object, imported, local, bytes);
		if (result != (uint32_t)VK_SUCCESS)
			ghci_vk_object_destroy(session, object);
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, memory);

	/* Succeeded: the memory exists, or the reply says why not. */
	return 0;
}

/*
 * Gives a memory object its memory (a new allocation of host memory, or
 * of local memory when local is set, or the imported resource's) and maps
 * it into the device's GHCI session; local memory is cleared by the GPU
 * before the object is the client's.  Returns the VkResult, which is
 * never device lost (vkAllocateMemory does not report it): a lost session
 * or device gives out of device memory.  On a failure the object holds
 * what it got, which its destroy lets go once the GPU no longer reaches
 * it.
 */
static uint32_t
memory_back(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object,
	uint32_t imported,
	int local,
	uint64_t bytes)
{
	struct ghci_allocation *allocation;
	struct ghci_resource *resource;
	struct ghci_device *device;
	int error;

	/* The memory: the resource of the import, or a new allocation below the GPU's limit. */
	device = session->device;
	allocation = NULL;
	if (imported != 0U) {
		mutex_lock(&device->lock);

		for (resource = session->owner->resources; resource != NULL; resource = resource->next) {
			if (resource->resource_id == imported)
				break;
		}
		if (resource != NULL && resource->allocation->bytes >= bytes) {
			allocation = resource->allocation;
			ghci_memory_hold(allocation);
		}

		mutex_unlock(&device->lock);
		if (allocation == NULL)
			return (uint32_t)VK_ERROR_INVALID_EXTERNAL_HANDLE;
		object->as.memory.imported = 1;
	} else {
		if (local)
			error = ghci_memory_alloc_local(device, bytes, &allocation);
		else
			error = ghci_memory_alloc(device, bytes, 0U, &allocation);
		if (error != 0)
			return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
		mutex_lock(&device->lock);

		ghci_memory_hold(allocation);

		mutex_unlock(&device->lock);
	}
	object->as.memory.allocation = allocation;
	object->as.memory.bytes = bytes;

	/* The mapping into the device's session: no GPU address left, or a lost session. */
	error = ghci_vm_map(device, object->device->vm, allocation, 0U, &object->as.memory.mapping);
	if (error != 0)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* New local memory is cleared before the client has it. */
	if (allocation->kind == GHCI_MEMORY_LOCAL && imported == 0U)
		return local_clear(session, object);

	/* Succeeded: the memory is the GPU's at its address in the session. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Clears a memory object's new local memory to zero with the GPU in the
 * device's session, and waits for it: the client can neither read the
 * memory with the GPU before work placed after the clear, nor see what
 * an earlier client left.  The wait has no deadline: it ends when the
 * clear's ticket passes, or when a reset or the loss of the device ends
 * it.  It holds no lock, so a busy GPU, a recovery and the display go on
 * meanwhile.  A clear that passed in a session the reset found a culprit
 * (whose work the GPU may have skipped), or on a device lost since, does
 * not count.
 *
 * Returns the VkResult: success, or out of device memory when the clear
 * cannot be made or did not clear (vkAllocateMemory does not report a
 * lost device).  On a failure the caller's destroy of the object gives
 * the run back only after its unmap's ticket, once the GPU no longer
 * reaches it.
 */
static uint32_t
local_clear(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object)
{
	struct ghci_vk_submission submission;
	struct ghci_vk_device *device;
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	enum ghci_device_state state;
	enum ghci_vm_state vm_state;
	unsigned long irq;
	int placed;
	int error;

	/* The packets that clear the mapping's bytes. */
	device = object->device;
	mapping = object->as.memory.mapping;
	kern_memset(&submission, 0, sizeof(submission));
	error = ghci_vk_execute_clear(session, device, mapping->va, mapping->bytes, &submission);
	if (error != 0) {
		ghci_vk_submission_end(device, &submission, NULL, 0);
		kern_logf("ghci: %s: vk: local memory not cleared (%d)\n", session->device->name, error);
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/*
	 * The clear and its fence in the device's session.  A clear placed
	 * in part before a recovery keeps its memory until the reset ends
	 * its ticket, as a submission does.
	 */
	error = ghci_cp_submit_placed(session->device, device->vm, submission.words, submission.used, &ticket, &placed);
	if (placed) {
		ghci_vk_submitted(device, &ticket);
		ghci_vk_submission_end(device, &submission, &ticket, 1);
	} else {
		ghci_vk_submission_end(device, &submission, NULL, 0);
	}
	if (error != 0)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The zeroes are in memory before the client has it: the ticket passes, or a reset ends it. */
	for (;;) {
		error = ghci_cp_wait(session->device, &ticket, CLEAR_WAIT_MS);
		if (error != ETIMEDOUT)
			break;
	}
	if (error != 0)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The session the clear ran in, under the device's lock (it changes under that lock and cp_lock). */
	mutex_lock(&session->device->lock);

	vm_state = device->vm->state;

	mutex_unlock(&session->device->lock);

	/* The device's life, as the recovery last set it. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	state = session->device->state;

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* A culprit's clear may have been skipped though its fence was written; a lost device's is not trusted. */
	if (vm_state == GHCI_VM_FAULTED || vm_state == GHCI_VM_DEAD || state == GHCI_STATE_LOST)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Succeeded: the memory holds zeroes. */
	return (uint32_t)VK_SUCCESS;
}

/* vkFreeMemory, vkDestroyBuffer and vkDestroyImage: the destroy form, no reply body. */
static int
free_memory(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t device;

	/* The destroy form. */
	identity = ghci_vk_destroy_form(reader, &device);
	if (reader->error != 0)
		return EINVAL;

	/* The object, when the session has it (the null handle destroys nothing). */
	object = ghci_vk_object_find(session, kind, identity);
	if (object != NULL)
		ghci_vk_object_destroy(session, object);

	/* Succeeded: the object is gone. */
	return 0;
}

/* vkGetDeviceMemoryCommitment: [device][memory][u64 1] -> [u64 1][u64 bytes]: all of it. */
static int
commitment(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	uint64_t memory;

	/* The device, the memory and the presence word. */
	(void)drv_i915_wire_read_u64(reader);
	memory = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A memory object of the session. */
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, memory);
	if (object == NULL)
		return EINVAL;

	/* Every byte is committed. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, object->as.memory.bytes);
	}

	/* Succeeded: the commitment is reported. */
	return 0;
}

/* vkCreateBuffer: the create form with VkBufferCreateInfo. */
static int
create_buffer(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t buffer;
	uint32_t result;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	buffer = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* No sparse buffer, and one no larger than the heap. */
	result = (uint32_t)VK_SUCCESS;
	if (info.size == 0U || info.size > ghci_vk_caps_heap_bytes(&session->device->caps))
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	else if ((info.flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT)) != 0U)
		result = (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* The object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_BUFFER, buffer, device, &object);
		if (error != 0)
			return error;
		object->as.buffer.size = info.size;
		object->as.buffer.usage = info.usage;
		object->as.buffer.flags = info.flags;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, buffer);

	/* Succeeded: the buffer exists, or the reply says why not. */
	return 0;
}

/* vkCreateImage: the create form with VkImageCreateInfo. */
static int
create_image(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct ghci_vk_image image;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkImageCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* What the image may be, and its layout. */
	kern_memset(&image, 0, sizeof(image));
	result = image_admit(session, &info, &image);

	/* The object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_IMAGE, identity, device, &object);
		if (error != 0)
			return error;
		object->as.image = image;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, identity);

	/* Succeeded: the image exists, or the reply says why not. */
	return 0;
}

/*
 * Checks an image against what its format, type, tiling, usage and flags
 * allow (the same answer vkGetPhysicalDeviceImageFormatProperties gives)
 * and fills its record and linear layout.  Returns the VkResult.
 */
static uint32_t
image_admit(
	struct ghci_vk_session *session,
	const VkImageCreateInfo *info,
	struct ghci_vk_image *image)
{
	VkImageFormatProperties limits;
	struct ghci_vk *vk;
	uint64_t bound;
	uint32_t result;
	uint32_t bytes;
	uint32_t block;
	int error;

	/* What the format, type, tiling, usage and flags allow. */
	vk = session->device->vk;
	result = ghci_vk_caps_image_format(&session->device->caps, &vk->formats, info->format, info->imageType, info->tiling, info->usage, info->flags, &limits);
	if (result != (uint32_t)VK_SUCCESS)
		return result;

	/* The extent, the levels, the layers and the samples within them. */
	if (info->extent.width == 0U || info->extent.height == 0U || info->extent.depth == 0U)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->extent.width > limits.maxExtent.width || info->extent.height > limits.maxExtent.height)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->extent.depth > limits.maxExtent.depth)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->mipLevels == 0U || info->mipLevels > limits.maxMipLevels)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->arrayLayers == 0U || info->arrayLayers > limits.maxArrayLayers)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((info->samples & limits.sampleCounts) == 0U || (info->samples & (info->samples - 1U)) != 0U)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* A bound on the bytes before the layout is computed in 32 bits: every level twice the first is more. */
	error = ghci_vk_format_size(info->format, &bytes, &block);
	if (error != 0)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	bound = ((uint64_t)info->extent.width * info->samples * bytes + GHCI_LAYOUT_ALIGN) * 2U;
	bound *= (uint64_t)info->extent.height * info->extent.depth * info->arrayLayers;
	if (bound > ghci_vk_caps_heap_bytes(&session->device->caps) * 2U)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The record and the linear layout. */
	image->format = info->format;
	image->type = info->imageType;
	image->tiling = info->tiling;
	image->usage = info->usage;
	image->flags = info->flags;
	ghci_vk_image_layout(info->extent.width,
			     info->extent.height,
			     info->extent.depth,
			     info->mipLevels,
			     info->arrayLayers,
			     info->samples,
			     bytes,
			     block,
			     &image->layout);
	image->bytes = ghci_layout_size(&image->layout);
	if (image->bytes > ghci_vk_caps_heap_bytes(&session->device->caps))
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Succeeded: the image may be made. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * vkGetBufferMemoryRequirements and vkGetImageMemoryRequirements:
 * [device][object][u64 1] -> [u64 1][VkMemoryRequirements]: the size,
 * 256-byte alignment, and every memory type of the device.
 */
static int
requirements(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkMemoryRequirements record;
	struct ghci_vk_object *object;
	uint64_t identity;

	/* The device, the object and the presence word. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An object of the session. */
	object = ghci_vk_object_find(session, kind, identity);
	if (object == NULL)
		return EINVAL;

	/* Its bytes, rounded, at the alignment of every buffer and image. */
	kern_memset(&record, 0, sizeof(record));
	if (kind == GHCI_VK_BUFFER)
		record.size = round_up(object->as.buffer.size, BUFFER_GRANULE);
	else
		record.size = round_up(object->as.image.bytes, GHCI_VK_ALIGN);
	record.alignment = GHCI_VK_ALIGN;
	record.memoryTypeBits = (1U << memory_types(session)) - 1U;

	/* The presence word and the record. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkMemoryRequirements(reply, &record);
	}

	/* Succeeded: the requirements are reported. */
	return 0;
}

/*
 * vkBindBufferMemory and vkBindImageMemory: [device][object][memory]
 * [u64 offset] -> [result].  The object lies wholly in the memory at an
 * aligned offset, once.
 */
static int
bind(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	struct ghci_vk_object *memory;
	uint64_t *bound;
	uint64_t identity;
	uint64_t handle;
	uint64_t offset;
	uint64_t bytes;
	uint64_t *at;
	uint32_t result;

	/* The device, the object, the memory and the offset. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	handle = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The object and the memory, of the session and of one device. */
	object = ghci_vk_object_find(session, kind, identity);
	memory = ghci_vk_object_find(session, GHCI_VK_MEMORY, handle);
	if (object == NULL || memory == NULL || object->device != memory->device)
		return EINVAL;

	/* Where the binding is kept, and the bytes it needs. */
	if (kind == GHCI_VK_BUFFER) {
		bound = &object->as.buffer.memory;
		at = &object->as.buffer.offset;
		bytes = object->as.buffer.size;
	} else {
		bound = &object->as.image.memory;
		at = &object->as.image.offset;
		bytes = object->as.image.bytes;
	}

	/* Once, aligned, and inside the memory. */
	result = (uint32_t)VK_SUCCESS;
	if (*bound != 0U || (offset % GHCI_VK_ALIGN) != 0U)
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	else if (offset > memory->as.memory.bytes || bytes > memory->as.memory.bytes - offset)
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (result == (uint32_t)VK_SUCCESS) {
		*bound = handle;
		*at = offset;
	}

	/* The result. */
	ghci_vk_reply_result(reply, requested, result);

	/* Succeeded: the object is bound, or the reply says why not. */
	return 0;
}

/*
 * vkGetImageSubresourceLayout: [device][image][u64 1][VkImageSubresource]
 * [u64 1] -> [u64 1][VkSubresourceLayout] of the linear layout.
 */
static int
subresource_layout(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageSubresource subresource;
	VkSubresourceLayout record;
	struct ghci_vk_object *object;
	struct ghci_layout_image *layout;
	uint64_t identity;
	uint64_t present;

	/* The device, the image, the subresource and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	kern_memset(&subresource, 0, sizeof(subresource));
	if (present == 1U)
		i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;

	/* An image of the session, and a level and layer it has. */
	object = ghci_vk_object_find(session, GHCI_VK_IMAGE, identity);
	if (object == NULL)
		return EINVAL;
	layout = &object->as.image.layout;
	if (subresource.mipLevel >= layout->levels || subresource.arrayLayer >= layout->layers)
		return EINVAL;

	/* The level's place in the layer's, and its pitches. */
	kern_memset(&record, 0, sizeof(record));
	record.offset = (uint64_t)subresource.arrayLayer * ghci_layout_layer_stride(layout) +
	    ghci_layout_level_offset(layout, subresource.mipLevel);
	record.size = ghci_layout_level_bytes(layout, subresource.mipLevel);
	record.rowPitch = ghci_layout_row(layout, subresource.mipLevel);
	record.arrayPitch = ghci_layout_layer_stride(layout);
	record.depthPitch = ghci_layout_plane(layout, subresource.mipLevel);

	/* The presence word and the record. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_vkc_enc_VkSubresourceLayout(reply, &record);
	}

	/* Succeeded: the layout is reported. */
	return 0;
}

/*
 * vkGetImageSparseMemoryRequirements: [device][image][u64 1][u32 capacity]
 * [u64 capacity] -> [u64 1][u32 0][u64 0]: no sparse image.
 */
static int
sparse_requirements(
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	/* The device, the image, the presence word and the capacities. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* None. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u64(reply, 0U);
	}

	/* Succeeded: no requirement. */
	return 0;
}

/* Finds a device of the session by its identity; NULL when it has none. */
static struct ghci_vk_device *
device_of(
	struct ghci_vk_session *session,
	uint64_t handle)
{
	struct ghci_vk_object *object;

	/* The device's object, and its record. */
	object = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (object == NULL)
		return NULL;

	/* Succeeded: the record. */
	return object->device;
}

/* Reports a value rounded up to a granule (a power of two). */
static uint64_t
round_up(
	uint64_t value,
	uint64_t granule)
{
	/* The next multiple. */
	return (value + granule - 1U) & ~(granule - 1U);
}

/*
 * Adds a block to one of a device's executor heaps: twice that heap's
 * bytes (the first is GHCI_VK_HEAP_FIRST_BYTES), at least least bytes, at
 * most GHCI_VK_HEAP_BLOCK_MAX, mapped into the device's session (the
 * code heap executable and read-only to the GPU, the data heap writable
 * and not executable).  Returns 0, ENOSPC when the GPU's memory, its
 * addresses or its page-table memory is used up (or the block would pass
 * GHCI_VK_HEAP_BLOCK_MAX), ENOMEM when the kernel has no memory for the
 * block's record, or EIO for a session or a device the GPU lost.
 */
static int
heap_grow(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	enum ghci_vk_heap heap,
	uint32_t least)
{
	struct ghci_vk_heap_block *block;
	struct ghci_allocation *allocation;
	uint64_t bytes;
	uint32_t flags;
	int error;

	/* The block's size. */
	bytes = GHCI_VK_HEAP_FIRST_BYTES;
	if (device->heap_bytes[heap] != 0U)
		bytes = device->heap_bytes[heap];
	while (bytes < least)
		bytes <<= 1;
	if (bytes > GHCI_VK_HEAP_BLOCK_MAX)
		return ENOSPC;

	/* The record. */
	block = kern_calloc(1U, sizeof(*block));
	if (block == NULL)
		return ENOMEM;

	/* The memory, held by the block. */
	error = ghci_memory_alloc(session->device, bytes, 0U, &allocation);
	if (error != 0) {
		kern_free(block);
		if (error == ENOMEM)
			return ENOSPC;
		return error;
	}
	mutex_lock(&session->device->lock);

	ghci_memory_hold(allocation);

	mutex_unlock(&session->device->lock);

	/*
	 * Its mapping in the device's session: the code heap executable and
	 * not writable by the GPU (it only reads records, code and tables
	 * the host wrote), the data heap writable and not executable; a
	 * failure lets the memory go again.
	 */
	flags = 0U;
	if (heap == GHCI_VK_HEAP_CODE)
		flags = GHCI_VM_FLAG_EXEC | GHCI_VM_MAP_NO_WRITE;
	error = ghci_vm_map(session->device, device->vm, allocation, flags, &block->mapping);
	if (error != 0) {
		mutex_lock(&session->device->lock);

		ghci_memory_unhold(session->device, allocation);

		mutex_unlock(&session->device->lock);
		kern_free(block);
		if (error == ENOMEM)
			return ENOSPC;
		return error;
	}

	/* The block's allocator, and the block first in its heap. */
	block->allocation = allocation;
	ghci_range_init(&block->map, 0U, bytes, block->extents, GHCI_VK_HEAP_EXTENTS);
	block->next = device->heap[heap];
	device->heap[heap] = block;
	device->heap_bytes[heap] += bytes;

	/* Succeeded: the heap has room. */
	return 0;
}

/* Reports the memory types the device answers: two on a discrete GPU whose local memory is in use, else one. */
static uint32_t
memory_types(
	struct ghci_vk_session *session)
{
	/* A discrete GPU: local memory and host memory. */
	if (ghci_memory_local_bytes(session->device) != 0U)
		return 2U;

	/* Succeeded: one type, in host memory. */
	return 1U;
}
