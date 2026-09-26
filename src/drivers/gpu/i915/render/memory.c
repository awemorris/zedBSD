/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's device memory and buffers (see memory.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as read from the
 * library's own senders (memory.c and resources.c of libvulkan).
 */

#include "memory.h"
#include "codec.h"
#include "gfx.h"
#include "internal.h"
#include "object.h"
#include "render.h"
#include "reply.h"
#include "../i915.h"
#include "../memory.h"
#include "../session.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vulkan-codec.inc"

static int i915_gfx_bind_image(struct i915_render_session *session, struct i915_gfx_memory *memory, uint64_t resource, uint64_t offset);
static int i915_gfx_bind_buffer(struct i915_render_session *session, struct i915_gfx_memory *memory, uint64_t resource, uint64_t offset);
static struct i915_gem_object *memory_import_object(struct i915_render_session *session, uint32_t resource);

/*
 * Every live VkDeviceMemory, newest first.
 *
 * A blob finds the allocation it is the storage of here, by the executor
 * device, the open of the node and the allocation's identity (every process
 * numbers its allocations from the same start).  An allocation joins the list once
 * it is published and leaves it on vkFreeMemory.  The list takes no lock of
 * its own; it relies on its callers not running an executor command and a
 * blob attach or detach at once.  XXX: the list is shared by every executor
 * device and session rather than kept by its owner.
 */
static struct i915_gfx_memory *i915_gfx_memories;

/*
 * Returns the CPU view of `bytes` bytes of a memory range.
 *
 * NULL when the memory has no storage yet or the range does not fit the
 * storage.
 */
uint8_t *
drv_i915_gfx_memory_cpu(
	struct i915_gfx_memory *memory,
	uint64_t offset,
	uint64_t bytes)
{
	uint8_t *address;

	/* An absent allocation has no view. */
	if (memory == NULL)
		return NULL;

	/* An allocation whose blob has not arrived has no storage yet. */
	if (memory->object == NULL)
		return NULL;

	/* Refuses a range that starts beyond the storage. */
	if (offset > memory->object->bytes)
		return NULL;

	/* Refuses a range that runs past the end of the storage. */
	if (bytes > memory->object->bytes - offset)
		return NULL;

	/* Finds the storage in the kernel direct map. */
	address = kern_pmem_to_kernel(memory->object->run.paddr);

	/* Succeeded: reports the view of the range. */
	return address + offset;
}

/*
 * Returns the GPU address of a memory range in the session's address space.
 *
 * 0 when the memory has no storage or the storage is not bound.
 */
uint64_t
drv_i915_gfx_memory_va(
	struct i915_gfx_memory *memory,
	uint64_t offset)
{
	/* An absent allocation has no address. */
	if (memory == NULL)
		return 0U;

	/* An allocation whose blob has not arrived has no storage yet. */
	if (memory->object == NULL)
		return 0U;

	/* Storage that is not bound into the session's address space has no address. */
	if (memory->object->va == 0U)
		return 0U;

	/* Succeeded: reports the address of the range. */
	return memory->object->va + offset;
}

/*
 * Attaches a blob as the storage of the allocation it names.
 *
 * The blob libvulkan creates for an allocation names it by `blob_id`: that
 * blob is the allocation's storage, which the application maps and the GPU
 * addresses.  Returns EINVAL when the allocation already has storage or the
 * blob is smaller than the allocation, and ENOENT when no allocation has that
 * identity in the open that made the blob.
 */
int
drv_i915_render_blob_attach(
	struct i915_render_device *vk,
	struct i915_session *gpu,
	uint64_t blob_id,
	struct i915_gem_object *object)
{
	struct i915_gfx_memory *memory;

	/* Looks for the allocation of this device and open that the blob names. */
	for (memory = i915_gfx_memories; memory != NULL; memory = memory->next) {
		/* Stops at an allocation of this device and open with the blob's identity. */
		if (memory->vk == vk && memory->gpu == gpu && memory->identity == blob_id)
			break;
	}

	/* No allocation has that identity. */
	if (memory == NULL)
		return ENOENT;

	/* Refuses a second storage for the same allocation. */
	if (memory->object != NULL)
		return EINVAL;

	/* Refuses a blob that cannot hold the whole allocation. */
	if (object->bytes < memory->size)
		return EINVAL;

	/* Succeeded: the blob is the allocation's storage from here on. */
	memory->object = object;
	return 0;
}

/*
 * Detaches a blob that is going away from the allocations it was the
 * storage of.
 *
 * Whatever still names the blob has no storage from here on.
 */
void
drv_i915_render_blob_detach(
	struct i915_render_device *vk,
	struct i915_gem_object *object)
{
	struct i915_gfx_memory *memory;

	/* Forgets the blob in every allocation of this device it backs. */
	for (memory = i915_gfx_memories; memory != NULL; memory = memory->next) {
		/* An allocation of this device whose storage is the blob loses it. */
		if (memory->vk == vk && memory->object == object)
			memory->object = NULL;
	}
}

/*
 * Frees an allocation whose identity is already withdrawn.
 *
 * It leaves the list the blob attach searches first, so no blob becomes its
 * storage afterwards.  XXX: buffers and images bound to it keep a dangling
 * pointer; the application frees them first.
 */
void
drv_i915_gfx_memory_release(
	struct i915_gfx_memory *memory)
{
	struct i915_gfx_memory **link;

	/* Takes the allocation off the list where it is found. */
	for (link = &i915_gfx_memories;
	     *link != NULL;
	     link = &(*link)->next) {
		if (*link == memory) {
			*link = memory->next;
			break;
		}
	}

	/* Frees the record; the storage is the blob's, which its open releases. */
	kern_free(memory);
}

/*
 * The sType libvulkan chains for an import: VkImportMemoryResourceInfoMESA,
 * whose u32 is the id of a resource the session imported (GPU_RESOURCE_IMPORT).
 */
#define I915_VK_IMPORT_MEMORY_RESOURCE	1000384002U

/*
 * Finds the object of a session with a resource id: the alias an import made
 * of another open's object (resource.c), bound into this session's address
 * space.  NULL when the session has no such resource.
 */
static struct i915_gem_object *
memory_import_object(
	struct i915_render_session *session,
	uint32_t resource)
{
	struct i915_gem_object *object;
	struct i915_device *device;

	/* The session's objects change under the device mutex. */
	device = session->vk->i915;
	mutex_lock(&device->mutex);

	/* The object with that slot. */
	for (object = session->gpu->objects; object != NULL; object = object->session_next) {
		if (object->slot == resource)
			break;
	}

	/* The list may change again. */
	mutex_unlock(&device->mutex);

	/* The object, or NULL. */
	return object;
}

/*
 * Allocates a VkDeviceMemory: vkAllocateMemory.
 *
 * The command is [device][present][sType][chain present]{[sType]
 * [pNext = 0][u32]}[allocationSize][memoryTypeIndex][pAllocator][present]
 * [identity] and the reply [result][present][identity].  Only memory type 0
 * and a nonzero size are accepted.  An ordinary allocation has no storage
 * until its blob arrives.  An import (VkImportMemoryResourceInfoMESA) takes
 * as its storage the session's resource it names: an alias of another open's
 * object, such as a Wayland client's image the compositor samples
 * (ws035-p066).  An export declaration needs nothing here: sharing is by the
 * node's resources.
 */
int
drv_i915_gfx_allocate_memory(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_memory *memory;
	struct i915_gfx_memory *published;
	struct i915_gem_object *imported;
	uint64_t chain;
	uint64_t size;
	uint64_t identity;
	uint32_t kind;
	uint32_t value;
	uint32_t resource;
	uint32_t type;
	int error;

	/* Skips the device, the create info's presence marker and its sType. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* Reads the presence marker of the one extension record libvulkan may chain. */
	chain = drv_i915_wire_read_u64(reader);

	/* An import names its resource; an export declaration is only read. */
	resource = 0U;
	if (chain != 0U) {
		kind = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		value = drv_i915_wire_read_u32(reader);
		if (kind == I915_VK_IMPORT_MEMORY_RESOURCE)
			resource = value;
	}

	/* Reads the size, the memory type and the identity. */
	size = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses an empty allocation and any memory type but the one the executor reports. */
	error = 0;
	if (size == 0U || type != 0U)
		error = EINVAL;

	/* An import's storage is its resource, which must hold the whole allocation. */
	imported = NULL;
	if (error == 0 && resource != 0U) {
		imported = memory_import_object(session, resource);
		if (imported == NULL) {
			kern_logf("i915: vk: import of resource %u refused: no such resource\n", resource);
			error = EINVAL;
		} else if (imported->bytes < size) {
			kern_logf("i915: vk: import of resource %u refused: smaller than the allocation\n", resource);
			error = EINVAL;
		}
	}

	/* Allocates the allocation's record for an accepted request. */
	memory = NULL;
	if (error == 0)
		memory = kern_calloc(1U, sizeof(*memory));

	/* Initializes the record; an ordinary allocation's storage arrives later as a blob. */
	if (memory != NULL) {
		memory->vk = session->vk;
		memory->gpu = session->gpu;
		memory->identity = identity;
		memory->size = size;
		memory->object = imported;
	}

	/* Publishes the allocation and answers; a record that is not published is freed. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_MEMORY, identity, memory, error);

	/*
	 * A published allocation joins the list the blob attach searches.  The
	 * table is asked rather than the error, because the reply helper owns
	 * the decision and frees what it does not publish.
	 */
	if (memory != NULL) {
		published = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, identity);
		if (published == memory) {
			memory->next = i915_gfx_memories;
			i915_gfx_memories = memory;
		}
	}

	/* Succeeded: the reply carries the result of the allocation. */
	return 0;
}

/*
 * Frees a VkDeviceMemory: vkFreeMemory.
 *
 * The command is [device][identity][pAllocator] and has no reply body.  An
 * unknown identity is not an error.
 */
int
drv_i915_gfx_free_memory(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_memory *memory;
	uint64_t identity;

	/* Reads the identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Resolves the allocation; freeing an unknown one does nothing. */
	memory = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, identity);
	if (memory == NULL)
		return 0;

	/* Unpublishes the allocation, then frees it. */
	drv_i915_object_remove(session, I915_VK_OBJ_MEMORY, identity);
	drv_i915_gfx_memory_release(memory);

	/* Succeeded: the allocation is gone. */
	return 0;
}

/*
 * Binds a buffer or an image to a range of an allocation:
 * vkBindBufferMemory and vkBindImageMemory.
 *
 * The command is [device][resource][memory][offset] and the reply [result].
 * `image` selects the image form.  The resource must fit the allocation from
 * the offset on.
 */
int
drv_i915_gfx_bind(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int image)
{
	struct i915_gfx_memory *memory;
	uint64_t resource;
	uint64_t memory_id;
	uint64_t offset;
	uint32_t result;
	int error;

	/* Reads the resource, the allocation and the offset behind the device. */
	(void)drv_i915_wire_read_u64(reader);
	resource = drv_i915_wire_read_u64(reader);
	memory_id = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Resolves the allocation; an unknown one fails the bind. */
	error = EINVAL;
	memory = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, memory_id);

	/* Binds the image or the buffer the command names. */
	if (memory != NULL) {
		if (image != 0) {
			error = i915_gfx_bind_image(session, memory, resource, offset);
		} else {
			error = i915_gfx_bind_buffer(session, memory, resource, offset);
		}
	}

	/* Writes the result of the bind. */
	result = drv_i915_gfx_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result of the bind. */
	return 0;
}

/*
 * Reports the memory requirements of a buffer or an image:
 * vkGetBufferMemoryRequirements and vkGetImageMemoryRequirements.
 *
 * The command is [device][resource][present] and the reply
 * [present][VkMemoryRequirements].  `image` selects the image form.  An
 * unknown resource needs no memory of any type.
 */
int
drv_i915_gfx_requirements(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int image)
{
	VkMemoryRequirements requirements;
	struct i915_gfx_image *image_target;
	struct i915_gfx_buffer *buffer_target;
	uint64_t resource;
	uint64_t bytes;

	/* Reads the resource between the device and the output's presence marker. */
	(void)drv_i915_wire_read_u64(reader);
	resource = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Finds how many bytes the image or the buffer occupies. */
	bytes = 0U;
	if (image != 0) {
		image_target = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE, resource);
		if (image_target != NULL)
			bytes = image_target->bytes;
	} else {
		buffer_target = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, resource);
		if (buffer_target != NULL)
			bytes = buffer_target->size;
	}

	/* Asks for whole pages: the storage of an allocation is a blob, and a blob is pages. */
	kern_memset(&requirements, 0, sizeof(requirements));
	requirements.size = (bytes + 4095U) & ~(uint64_t)4095U;
	requirements.alignment = 4096U;

	/* Memory type 0 serves every resource that needs memory at all. */
	requirements.memoryTypeBits = 0U;
	if (bytes != 0U)
		requirements.memoryTypeBits = 1U;

	/* Writes the requirements behind their presence marker. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkMemoryRequirements(reply, &requirements);

	/* Succeeded: the reply carries the requirements. */
	return 0;
}

/*
 * Creates a VkBuffer: vkCreateBuffer, a generic create.
 *
 * The buffer keeps its size and usage; it has no storage until it is bound.
 */
int
drv_i915_gfx_create_buffer(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct i915_gfx_buffer *buffer;
	uint64_t identity;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Allocates the buffer and keeps what the draws and copies read of it. */
	buffer = kern_calloc(1U, sizeof(*buffer));
	if (buffer != NULL) {
		buffer->size = info.size;
		buffer->usage = info.usage;
	}

	/* Publishes the buffer and answers; a failed allocation is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_BUFFER, identity, buffer, 0);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/* Binds an image to a range of an allocation it fits in. */
static int
i915_gfx_bind_image(
	struct i915_render_session *session,
	struct i915_gfx_memory *memory,
	uint64_t resource,
	uint64_t offset)
{
	struct i915_gfx_image *image;

	/* Resolves the image; an unknown one fails the bind. */
	image = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE, resource);
	if (image == NULL)
		return EINVAL;

	/* Refuses an offset beyond the allocation. */
	if (offset > memory->size)
		return EINVAL;

	/* Refuses an image that runs past the end of the allocation. */
	if (image->bytes > memory->size - offset)
		return EINVAL;

	/* Succeeded: the image lives in the allocation from the offset on. */
	image->memory = memory;
	image->offset = offset;
	return 0;
}

/* Binds a buffer to a range of an allocation it fits in. */
static int
i915_gfx_bind_buffer(
	struct i915_render_session *session,
	struct i915_gfx_memory *memory,
	uint64_t resource,
	uint64_t offset)
{
	struct i915_gfx_buffer *buffer;

	/* Resolves the buffer; an unknown one fails the bind. */
	buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, resource);
	if (buffer == NULL)
		return EINVAL;

	/* Refuses an offset beyond the allocation. */
	if (offset > memory->size)
		return EINVAL;

	/* Refuses a buffer that runs past the end of the allocation. */
	if (buffer->size > memory->size - offset)
		return EINVAL;

	/* Succeeded: the buffer lives in the allocation from the offset on. */
	buffer->memory = memory;
	buffer->offset = offset;
	return 0;
}
