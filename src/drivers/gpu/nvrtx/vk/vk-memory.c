/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's memory: VkDeviceMemory, the binding of buffers and images to
 * it, and what they require of it.
 *
 * An allocation is only a record until its storage arrives: libvulkan
 * creates a blob for every allocation of the one (host visible) memory
 * type right after the allocation, naming the allocation's identity, and
 * the node attaches the blob's VRAM to the record (nvrtx_vk_memory_attach).
 * An import takes as its storage a blob of the session's that already
 * exists (another open's image the compositor samples).  The storage goes
 * back to the node when nothing holds the allocation any more: not the
 * client, and no buffer or image bound to it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

static int memory_allocate(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int memory_commitment(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int memory_bind(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool image);
static int memory_bind_to(struct nvrtx_vk_session *session, struct nvrtx_vk_object *memory, uint64_t resource, uint64_t offset, bool image);
static int memory_requirements(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool image);
static int memory_destroy(struct nvrtx_vk_session *session, void *payload);
static int memory_cpu(struct nvrtx_vk_session *session, struct nvrtx_vk_object *object, uint64_t offset, uint8_t **cpu);

/*
 * Runs an allocation, binding or requirements command; *handled stays
 * clear for an opcode of another handler's.
 */
int
nvrtx_vk_memory_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_ALLOCATE_MEMORY:
		error = memory_allocate(session, reader, reply);
		break;
	case GPU_OP_FREE_MEMORY:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_MEMORY);
		break;
	case GPU_OP_GET_DEVICE_MEMORY_COMMITMENT:
		error = memory_commitment(session, reader, reply);
		break;
	case GPU_OP_BIND_BUFFER_MEMORY:
		error = memory_bind(session, reader, reply, false);
		break;
	case GPU_OP_BIND_IMAGE_MEMORY:
		error = memory_bind(session, reader, reply, true);
		break;
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
		error = memory_requirements(session, reader, reply, false);
		break;
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
		error = memory_requirements(session, reader, reply, true);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * Attaches a blob's storage to the allocation it was created for (its
 * blob_id names the allocation): the CPU's view, the GPU's address and
 * the node's backing, whose reference the allocation now holds.  The
 * caller holds the node's lock.
 *
 * Returns EINVAL for an allocation that does not exist, already has its
 * storage, or is not the blob's size; the node then keeps its reference.
 */
int
nvrtx_vk_memory_attach(
	struct nvrtx_vk_session *session,
	uint64_t identity,
	uint64_t bytes,
	uint8_t *cpu,
	uint64_t va,
	void *backing)
{
	struct nvrtx_vk_memory *memory;

	/* The allocation the blob names, without storage yet, of the blob's size. */
	memory = nvrtx_vk_payload(session, I915_VK_OBJ_MEMORY, identity);
	if (memory == NULL)
		return EINVAL;
	if (memory->attached)
		return EINVAL;
	if (memory->bytes != bytes)
		return EINVAL;

	/* The allocation's storage from now on; its destruction gives the backing back. */
	memory->cpu = cpu;
	memory->va = va;
	memory->backing = backing;
	memory->attached = true;

	/* Succeeded: buffers and images bound to the allocation reach its storage. */
	return 0;
}

/*
 * Reports the GPU and CPU addresses of a bound buffer's first byte; EIO
 * when it is not bound or its memory has no storage.
 */
int
nvrtx_vk_buffer_address(
	const struct nvrtx_vk_buffer *buffer,
	uint64_t *va,
	uint8_t **cpu)
{
	const struct nvrtx_vk_memory *memory;

	/* A buffer bound to memory with storage. */
	if (buffer->memory == NULL)
		return EIO;
	memory = buffer->memory->payload;
	if (!memory->attached)
		return EIO;

	/* The memory's addresses moved by the buffer's offset (no CPU view while the storage is out of the window). */
	*va = memory->va + buffer->offset;
	*cpu = NULL;
	if (memory->cpu != NULL)
		*cpu = memory->cpu + buffer->offset;

	/* Succeeded: the buffer's addresses. */
	return 0;
}

/*
 * Reports the GPU and CPU addresses of a bound image's first byte; EIO
 * when it is not bound or its memory has no storage.
 */
int
nvrtx_vk_image_address(
	const struct nvrtx_vk_image *image,
	uint64_t *va,
	uint8_t **cpu)
{
	const struct nvrtx_vk_memory *memory;

	/* An image bound to memory with storage. */
	if (image->memory == NULL)
		return EIO;
	memory = image->memory->payload;
	if (!memory->attached)
		return EIO;

	/* The memory's addresses moved by the image's offset (no CPU view while the storage is out of the window). */
	*va = memory->va + image->offset;
	*cpu = NULL;
	if (memory->cpu != NULL)
		*cpu = memory->cpu + image->offset;

	/* Succeeded: the image's addresses. */
	return 0;
}

/*
 * Gives the CPU's view of a bound buffer's first byte, putting its memory's
 * storage into the CPU's window when it has no place there yet.  EIO when
 * the buffer is not bound or its memory has no storage, ENOSPC when the
 * window is full.
 */
int
nvrtx_vk_buffer_cpu(
	struct nvrtx_vk_session *session,
	const struct nvrtx_vk_buffer *buffer,
	uint8_t **cpu)
{
	/* The buffer's memory and its offset there. */
	return memory_cpu(session, buffer->memory, buffer->offset, cpu);
}

/*
 * Gives the CPU's view of a bound image's first byte, as
 * nvrtx_vk_buffer_cpu() does for a buffer.
 */
int
nvrtx_vk_image_cpu(
	struct nvrtx_vk_session *session,
	const struct nvrtx_vk_image *image,
	uint8_t **cpu)
{
	/* The image's memory and its offset there. */
	return memory_cpu(session, image->memory, image->offset, cpu);
}

/*
 * vkAllocateMemory: [device][present][sType][chain] with, when chain is 1,
 * [extension sType][next][value], then [size][type][allocator][present]
 * [identity] -> [result][present][identity].
 *
 * The extension is an export declaration (only read) or an import of a
 * blob of the session's (GPU_OP_RECORD_IMPORT_RESOURCE with its resource
 * id), whose storage the allocation takes at once.
 */
static int
memory_allocate(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct nvrtx_vk_memory *memory;
	uint64_t chain;
	uint64_t bytes;
	uint64_t identity;
	uint32_t extension;
	uint32_t value;
	uint32_t imported;
	uint32_t type;
	int error;

	/* Skips the device, the create info's presence word and its sType; reads whether an extension follows. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);

	/* The one extension: an import names its resource, an export is only read. */
	imported = 0U;
	if (chain != 0U) {
		extension = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		value = drv_i915_wire_read_u32(reader);
		if (extension == GPU_OP_RECORD_IMPORT_RESOURCE)
			imported = value;
	}

	/* The size, the memory type and the identity. */
	bytes = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A nonempty allocation of the one memory type. */
	error = 0;
	if (bytes == 0U) {
		error = EINVAL;
	} else if (type != NVRTX_VK_MEMORY_TYPE) {
		error = EINVAL;
	}

	/* The record. */
	memory = NULL;
	if (error == 0) {
		memory = kern_calloc(1U, sizeof(*memory));
		if (memory != NULL)
			memory->bytes = bytes;
	}

	/* An import takes its resource's storage now (the node maps it and holds its backing for the record). */
	if (memory != NULL && imported != 0U) {
		error = nvrtx_vk_memory_import(session->owner, imported, bytes, &memory->cpu, &memory->va, &memory->backing);
		if (error == 0)
			memory->attached = true;
	}

	/* Publishes the record and answers; a refused record is destroyed there. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_MEMORY, identity, memory, memory_destroy, error);

	/* Succeeded: the reply carries the allocation's result. */
	return 0;
}

/* vkGetDeviceMemoryCommitment: [device][memory][present] -> [present][bytes]; all of it once its storage is there. */
static int
memory_commitment(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct nvrtx_vk_memory *memory;
	uint64_t identity;
	uint64_t committed;

	/* The allocation between the device and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The whole allocation is committed once it has storage, none of it before. */
	committed = 0U;
	memory = nvrtx_vk_payload(session, I915_VK_OBJ_MEMORY, identity);
	if (memory != NULL && memory->attached)
		committed = memory->bytes;

	/* The bytes behind their presence word. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, committed);

	/* Succeeded: the reply carries the commitment. */
	return 0;
}

/*
 * vkBindBufferMemory and vkBindImageMemory: [device][resource][memory]
 * [offset] -> [result].  The resource is bound once, on the resource
 * boundary, inside the allocation.
 */
static int
memory_bind(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool image)
{
	struct nvrtx_vk_object *memory;
	uint64_t resource;
	uint64_t memory_id;
	uint64_t offset;
	uint32_t result;
	int error;

	/* The resource, the allocation and the offset behind the device. */
	(void)drv_i915_wire_read_u64(reader);
	resource = drv_i915_wire_read_u64(reader);
	memory_id = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Binds to an allocation that exists. */
	error = EINVAL;
	memory = nvrtx_vk_object_find(session, I915_VK_OBJ_MEMORY, memory_id);
	if (memory != NULL)
		error = memory_bind_to(session, memory, resource, offset, image);

	/* The bind's result. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * Binds a buffer or an image to an allocation at an offset: the resource
 * takes a reference to the allocation's object.
 */
static int
memory_bind_to(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_object *memory,
	uint64_t resource,
	uint64_t offset,
	bool image)
{
	const struct nvrtx_vk_memory *storage;
	struct nvrtx_vk_buffer *buffer;
	struct nvrtx_vk_image *picture;
	struct nvrtx_vk_object **holder;
	uint64_t *place;
	uint64_t bytes;
	uint64_t align;
	int error;

	/* The resource's size, its boundary (a GOB for a block-linear image) and where it keeps its binding. */
	align = NVRTX_VK_RESOURCE_ALIGN;
	if (image) {
		picture = nvrtx_vk_payload(session, I915_VK_OBJ_IMAGE, resource);
		if (picture == NULL)
			return EINVAL;
		bytes = picture->bytes;
		holder = &picture->memory;
		place = &picture->offset;
		if (picture->depth || picture->tiled)
			align = NVRTX_VK_GOB_BYTES;

		/* A depth image with a stencil on a page of its own. */
		if (picture->stencil)
			align = NVRTX_VK_KIND_ALIGN;

		/* A video image is placed on the decoder's boundary. */
		if (picture->video)
			align = NVRTX_VK_VIDEO_ALIGN;
	} else {
		buffer = nvrtx_vk_payload(session, I915_VK_OBJ_BUFFER, resource);
		if (buffer == NULL)
			return EINVAL;
		bytes = buffer->size;
		holder = &buffer->memory;
		place = &buffer->offset;
	}

	/* A resource is bound once. */
	if (*holder != NULL)
		return EINVAL;

	/* On the boundary, wholly inside the allocation. */
	storage = memory->payload;
	if ((offset % align) != 0U)
		return EINVAL;
	if (offset > storage->bytes)
		return EINVAL;
	if (bytes > storage->bytes - offset)
		return EINVAL;

	/* A colour image of one level is what a shared image is: its shape goes with the storage, and an exported one's shape is checked. */
	if (image && storage->attached && picture->levels == 1U && !picture->depth && !picture->video) {
		error = nvrtx_vk_memory_bind_image(storage->backing, picture->tiled, picture->pitch, picture->width, picture->height, offset);
		if (error != 0)
			return error;
	}

	/* The resource holds the allocation while it lives. */
	error = nvrtx_vk_object_retain(memory);
	if (error != 0)
		return error;

	*holder = memory;
	*place = offset;

	/* Succeeded: the resource reaches the allocation's storage from the offset. */
	return 0;
}

/*
 * vkGetBufferMemoryRequirements and vkGetImageMemoryRequirements:
 * [device][resource][present] -> [present][VkMemoryRequirements].  An
 * unknown resource needs no memory of any type.
 */
static int
memory_requirements(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool image)
{
	VkMemoryRequirements requirements;
	const struct nvrtx_vk_buffer *buffer;
	const struct nvrtx_vk_image *picture;
	uint64_t resource;
	uint64_t align;
	uint64_t bytes;

	/* The resource between the device and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	resource = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* How many bytes the resource occupies, and its boundary (a GOB for a block-linear image). */
	bytes = 0U;
	align = NVRTX_VK_RESOURCE_ALIGN;
	if (image) {
		picture = nvrtx_vk_payload(session, I915_VK_OBJ_IMAGE, resource);
		if (picture != NULL)
			bytes = picture->bytes;
		if (picture != NULL && (picture->depth || picture->tiled))
			align = NVRTX_VK_GOB_BYTES;

		/* A depth image with a stencil on a page of its own. */
		if (picture != NULL && picture->stencil)
			align = NVRTX_VK_KIND_ALIGN;

		/* A video image is placed on the decoder's boundary. */
		if (picture != NULL && picture->video)
			align = NVRTX_VK_VIDEO_ALIGN;
	} else {
		buffer = nvrtx_vk_payload(session, I915_VK_OBJ_BUFFER, resource);
		if (buffer != NULL)
			bytes = buffer->size;
	}

	/* Whole runs of the boundary, on that boundary, of the one memory type. */
	kern_memset(&requirements, 0, sizeof(requirements));
	requirements.size = (bytes + align - 1U) & ~(align - 1U);
	requirements.alignment = align;
	requirements.memoryTypeBits = 0U;
	if (bytes != 0U)
		requirements.memoryTypeBits = 1U << NVRTX_VK_MEMORY_TYPE;

	/* The requirements behind their presence word. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkMemoryRequirements(reply, &requirements);

	/* Succeeded: the reply carries the requirements. */
	return 0;
}

/* Destroys an allocation nothing holds any more: its storage goes back to the node. */
static int
memory_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_memory *memory;

	memory = payload;

	/* The node unmaps the run and lets its backing go. */
	if (memory->attached)
		nvrtx_vk_memory_detach(session->owner, memory->backing, memory->va, memory->bytes);

	/* The record. */
	kern_free(memory);

	/* Succeeded: the allocation is gone. */
	return 0;
}

/*
 * Gives the CPU's view of the byte at offset in a memory object's storage,
 * mapping the storage into the window the first time (vk-node.c).
 */
static int
memory_cpu(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_object *object,
	uint64_t offset,
	uint8_t **cpu)
{
	struct nvrtx_vk_memory *memory;
	int error;

	/* A memory with storage. */
	if (object == NULL)
		return EIO;
	memory = object->payload;
	if (!memory->attached)
		return EIO;

	/* The storage's place in the window, kept for the next time. */
	if (memory->cpu == NULL) {
		error = nvrtx_vk_memory_cpu(session->owner, memory->backing, &memory->cpu);
		if (error != 0)
			return error;
	}

	/* Succeeded: the byte's place. */
	*cpu = memory->cpu + offset;
	return 0;
}
