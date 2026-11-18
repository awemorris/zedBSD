/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's memory and what binds to it: device memory (allocated,
 * or imported from a blob the session has), buffers, linear images and
 * their views, samplers, fences and semaphores.
 *
 * A VkDeviceMemory is a buffer object mapped in the session's space; a
 * buffer or an image is a range of one, and its GPU address is the
 * memory's plus its offset.  Every image is linear: rows of whole texels
 * whose pitch is a multiple of 64 bytes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-surface.h"
#include "drivers/gpu/mali/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The handle types an allocation may be exported by: an opaque file
 * descriptor and a dma-buf.
 */
#define EXTERNAL_HANDLES	0x201U

/*
 * A sampler's words: the type, the wrap modes, nearest or linear, the
 * mipmap mode; and the numbers of each.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Sampler": type 0..3,
 * wrap R 8..11, T 12..15, S 16..19, round to nearest even 21, seamless
 * cube 23, normalized coordinates 25, clamp array indices 26, minify
 * nearest 27, magnify nearest 28, mipmap mode 30..31; word 1 maximum LOD
 * 16..28; word 2 maximum anisotropy 16..20 less one; enums "Descriptor
 * Type" sampler 1, "Wrap Mode" repeat 8, clamp to edge 9, clamp to
 * border 11, mirrored repeat 12, "Mipmap Mode" nearest 0, none 1).
 */
#define SAMPLER_TYPE		1U
#define SAMPLER_WRAP_R_SHIFT	8U
#define SAMPLER_WRAP_T_SHIFT	12U
#define SAMPLER_WRAP_S_SHIFT	16U
#define SAMPLER_ROUND_EVEN	(1U << 21)
#define SAMPLER_SEAMLESS	(1U << 23)
#define SAMPLER_NORMALIZED	(1U << 25)
#define SAMPLER_CLAMP_ARRAY	(1U << 26)
#define SAMPLER_MIN_NEAREST	(1U << 27)
#define SAMPLER_MAG_NEAREST	(1U << 28)
#define SAMPLER_MIPMAP_NONE	(1U << 30)
#define WRAP_REPEAT		8U
#define WRAP_CLAMP_EDGE		9U
#define WRAP_CLAMP_BORDER	11U
#define WRAP_MIRRORED		12U

static int allocate(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int memory_destroy(struct mali_vk_session *session, void *payload);
static int free_memory(struct mali_vk_session *session, struct i915_wire_reader *reader);
static int commitment(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_buffer(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int buffer_destroy(struct mali_vk_session *session, void *payload);
static int create_image(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_destroy(struct mali_vk_session *session, void *payload);
static int requirements(struct mali_vk_session *session, bool image, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int bind(struct mali_vk_session *session, bool image, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int subresource_layout(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_view(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int view_destroy(struct mali_vk_session *session, void *payload);
static int create_sampler(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t wrap_mode(uint32_t mode);
static int create_sync(struct mali_vk_session *session, bool fence, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset_fences(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int fence_status(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t swizzle_channel(uint32_t component, uint32_t identity);

/* Runs the memory, buffer, image, view, sampler, fence and semaphore commands. */
int
mali_vk_memory_route(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_ALLOCATE_MEMORY:
		error = allocate(session, reader, reply);
		break;
	case GPU_OP_FREE_MEMORY:
		error = free_memory(session, reader);
		break;
	case GPU_OP_GET_DEVICE_MEMORY_COMMITMENT:
		error = commitment(session, reader, reply);
		break;
	case GPU_OP_CREATE_BUFFER:
		error = create_buffer(session, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_BUFFER);
		break;
	case GPU_OP_CREATE_IMAGE:
		error = create_image(session, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_IMAGE);
		break;
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
		error = requirements(session, false, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
		error = requirements(session, true, reader, reply);
		break;
	case GPU_OP_BIND_BUFFER_MEMORY:
		error = bind(session, false, reader, reply);
		break;
	case GPU_OP_BIND_IMAGE_MEMORY:
		error = bind(session, true, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT:
		error = subresource_layout(session, reader, reply);
		break;
	case GPU_OP_CREATE_IMAGE_VIEW:
		error = create_view(session, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE_VIEW:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_IMAGE_VIEW);
		break;
	case GPU_OP_CREATE_SAMPLER:
		error = create_sampler(session, reader, reply);
		break;
	case GPU_OP_DESTROY_SAMPLER:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_SAMPLER);
		break;
	case GPU_OP_CREATE_FENCE:
		error = create_sync(session, true, reader, reply);
		break;
	case GPU_OP_DESTROY_FENCE:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_FENCE);
		break;
	case GPU_OP_RESET_FENCES:
		error = reset_fences(session, reader, reply);
		break;
	case GPU_OP_GET_FENCE_STATUS:
		error = fence_status(session, reader, reply);
		break;
	case GPU_OP_CREATE_SEMAPHORE:
		error = create_sync(session, false, reader, reply);
		break;
	case GPU_OP_DESTROY_SEMAPHORE:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_SEMAPHORE);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Gives the buffer object of a VkDeviceMemory, retained, for the blob
 * libvulkan makes to map it.
 */
int
mali_vk_memory_bo(
	struct mali_vk_session *session,
	uint64_t identity,
	uint64_t bytes,
	struct mali_bo **bo)
{
	struct mali_vk_memory *memory;

	/* A memory object of at least the bytes asked. */
	*bo = NULL;
	memory = mali_vk_payload(session, I915_VK_OBJ_MEMORY, identity);
	if (memory == NULL || bytes > memory->bo->buffer.bytes)
		return EINVAL;

	/* Succeeded: the caller holds a reference of its own. */
	mali_bo_retain(session->owner->render, memory->bo);
	*bo = memory->bo;
	return 0;
}

/* Reports a bound buffer's GPU address, zero when unbound. */
uint64_t
mali_vk_buffer_address(
	const struct mali_vk_buffer *buffer)
{
	const struct mali_vk_memory *memory;

	/* No memory yet. */
	if (buffer == NULL || buffer->memory == NULL)
		return 0;

	/* The memory's address plus the offset. */
	memory = buffer->memory->payload;
	return memory->resource->gpu_address + buffer->offset;
}

/* Reports a bound image's GPU address, zero when unbound. */
uint64_t
mali_vk_image_address(
	const struct mali_vk_image *image)
{
	const struct mali_vk_memory *memory;

	/* No memory yet. */
	if (image == NULL || image->memory == NULL)
		return 0;

	/* The memory's address plus the offset. */
	memory = image->memory->payload;
	return memory->resource->gpu_address + image->offset;
}

/*
 * Reports the GPU address of a level of a layer of a bound image: the
 * layer's start (layer after layer) plus the level's offset in it; zero
 * when not bound or outside the image.
 */
uint64_t
mali_vk_image_level_address(
	const struct mali_vk_image *image,
	uint32_t level,
	uint32_t layer)
{
	uint64_t base;

	/* A bound image, a level and a layer of it. */
	base = mali_vk_image_address(image);
	if (base == 0U || level >= image->levels || layer >= image->layers)
		return 0;

	/* The layer's start and the level's offset. */
	return base + (uint64_t)layer * image->layer_bytes + image->level[level].offset;
}

/* Reports the row pitch of an image's level, zero outside the image. */
uint32_t
mali_vk_image_level_pitch(
	const struct mali_vk_image *image,
	uint32_t level)
{
	/* A level of the image. */
	if (image == NULL || level >= image->levels)
		return 0;

	/* Its pitch. */
	return image->level[level].pitch;
}

/*
 * vkAllocateMemory: [device][present][sType][chain] (an import or export
 * record when chained) [size][type][allocator][present][identity].
 */
static int
allocate(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_memory *memory;
	struct mali_resource *resource;
	uint64_t chain;
	uint64_t bytes;
	uint64_t created;
	uint32_t extension;
	uint32_t value;
	uint32_t type;
	int error;

	/* The device, the info's presence and type, and its chain. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	extension = 0;
	value = 0;
	if (chain != 0U) {
		extension = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		value = drv_i915_wire_read_u32(reader);
	}

	/* The size, the type and the tail. */
	bytes = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A taken type and extension; one larger than a buffer object the node makes is out of device memory. */
	error = 0;
	if (type >= MALI_VK_MEMORY_TYPES || bytes == 0U)
		error = EINVAL;
	else if (bytes > MALI_RENDER_MAX_BYTES)
		error = ENOSPC;
	if (chain != 0U && extension == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO && (value == 0U || (value & ~EXTERNAL_HANDLES) != 0U))
		error = ENOTSUP;
	if (chain != 0U && extension != VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO && extension != GPU_OP_RECORD_IMPORT_RESOURCE)
		error = ENOTSUP;
	if (error != 0) {
		mali_vk_create_reply(session, reply, I915_VK_OBJ_MEMORY, created, NULL, memory_destroy, error);
		return 0;
	}

	/* The payload. */
	memory = kern_calloc(1, sizeof(*memory));
	if (memory == NULL) {
		mali_vk_create_reply(session, reply, I915_VK_OBJ_MEMORY, created, NULL, memory_destroy, ENOMEM);
		return 0;
	}
	memory->bytes = bytes;
	memory->type = type;

	/* An imported blob lends its object and its mapping in the session. */
	if (chain != 0U && extension == GPU_OP_RECORD_IMPORT_RESOURCE) {
		resource = mali_session_blob(session->owner, value);
		if (resource == NULL || bytes > resource->bo->buffer.bytes) {
			error = EINVAL;
		} else {
			mali_bo_retain(session->owner->render, resource->bo);
			memory->bo = resource->bo;
			memory->resource = resource;
		}
	} else {
		error = mali_bo_create(session->owner->render, bytes, &memory->bo);
		if (error == 0)
			error = mali_resource_make(session->owner, memory->bo, true, &memory->resource);
		if (error != 0 && memory->bo != NULL) {
			mali_bo_put(session->owner->render, memory->bo);
			memory->bo = NULL;
		}
	}

	if (error != 0) {
		kern_free(memory);
		mali_vk_create_reply(session, reply, I915_VK_OBJ_MEMORY, created, NULL, memory_destroy, error);
		return 0;
	}

	/* Succeeded: the memory is published. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_MEMORY, created, memory, memory_destroy, 0);
	return 0;
}

/*
 * Destroys a memory object: its own mapping goes (an imported one
 * stays the blob's), then its reference on the object.
 */
static int
memory_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_memory *memory;
	struct mali_resource *resource;
	struct mali_resource *walk;
	bool owned;

	/* Only a mapping the memory made itself is unmade; an imported blob's handle is the core's. */
	memory = payload;
	resource = memory->resource;
	owned = false;
	for (walk = session->owner->resources; walk != NULL; walk = walk->next) {
		if (walk == resource && resource->identifier == 0U)
			owned = true;
	}

	if (owned)
		mali_resource_unmake(session->owner, resource);
	mali_bo_put(session->owner->render, memory->bo);
	kern_free(memory);

	/* Succeeded: the memory is gone. */
	return 0;
}

/* vkFreeMemory: [device][memory][allocator]. */
static int
free_memory(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	uint64_t gone;

	/* The memory between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	gone = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublished; buffers and images bound to it keep it until they go. */
	(void)mali_vk_object_remove(session, I915_VK_OBJ_MEMORY, gone);
	return 0;
}

/* vkGetDeviceMemoryCommitment: [device][memory][present] -> [present][bytes]: all of it. */
static int
commitment(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_memory *memory;
	uint64_t named;

	/* The memory. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Every byte is backed. */
	memory = mali_vk_payload(session, I915_VK_OBJ_MEMORY, named);
	drv_i915_wire_reply_u64(reply, 1);
	if (memory == NULL) {
		drv_i915_wire_reply_u64(reply, 0);
	} else {
		drv_i915_wire_reply_u64(reply, memory->bytes);
	}

	/* Succeeded: the reply. */
	return 0;
}

/* vkCreateBuffer: the create form with VkBufferCreateInfo. */
static int
create_buffer(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct mali_vk_buffer *buffer;
	uint64_t present;
	uint64_t created;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A buffer of some bytes, not sparse. */
	error = 0;
	buffer = NULL;
	if (info.size == 0U || info.size > MALI_RENDER_MAX_BYTES || (info.flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) != 0U)
		error = ENOTSUP;
	if (error == 0) {
		buffer = kern_calloc(1, sizeof(*buffer));
		if (buffer != NULL) {
			buffer->size = info.size;
			buffer->usage = info.usage;
		}
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_BUFFER, created, buffer, buffer_destroy, error);
	return 0;
}

/* Destroys a buffer: its hold on its memory goes. */
static int
buffer_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_buffer *buffer;

	(void)session;
	buffer = payload;
	mali_vk_object_release(buffer->memory);
	kern_free(buffer);
	return 0;
}

/*
 * vkCreateImage: a 2D image of a taken format, of levels down to one
 * texel at most, of one layer or an array of them (six or more of a
 * cube-compatible one), laid out linearly: layer after layer, each
 * layer its levels one after another.  A depth and stencil attachment
 * is of a format the tile buffer takes.
 */
static int
create_image(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct mali_vk_image *image;
	const struct mali_vk_format *format;
	uint64_t present;
	uint64_t created;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkImageCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A shape the executor takes. */
	error = 0;
	image = NULL;
	format = mali_vk_format_find(info.format);
	if (format == NULL || info.imageType != VK_IMAGE_TYPE_2D || info.extent.width == 0U || info.extent.height == 0U) {
		error = ENOTSUP;
	} else if (info.extent.width > MALI_VK_IMAGE_EDGE || info.extent.height > MALI_VK_IMAGE_EDGE) {
		error = ENOTSUP;
	} else if (info.mipLevels == 0U || info.mipLevels > mali_surface_level_count(info.extent.width, info.extent.height) || info.mipLevels > MALI_SURFACE_LEVELS) {
		/* Levels past the one-texel level are not an image's. */
		error = EINVAL;
	} else if (info.arrayLayers == 0U || info.arrayLayers > MALI_VK_IMAGE_LAYERS || info.samples != VK_SAMPLE_COUNT_1_BIT) {
		error = ENOTSUP;
	} else if ((info.flags & ~(uint32_t)(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) != 0U) {
		/* Another flag (sparse, aliased, 2D views of 3D) is not taken. */
		error = ENOTSUP;
	} else if ((info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U && (info.extent.width != info.extent.height || info.arrayLayers < 6U)) {
		/* A cube's faces are square, six of them at least. */
		error = EINVAL;
	} else if ((info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U && mali_surface_zs_find(info.format) == NULL) {
		/* A depth and stencil attachment of a format the tile buffer does not take. */
		error = ENOTSUP;
	} else if (format->depth && (info.usage & ~(uint32_t)(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0U) {
		/* A depth image is sampled, copied and a depth and stencil attachment, nothing else. */
		error = ENOTSUP;
	}

	/* The payload with its linear layout: the levels of a layer, the layers after one another. */
	if (error == 0) {
		image = kern_calloc(1, sizeof(*image));
		if (image != NULL) {
			image->format = format;
			image->width = info.extent.width;
			image->height = info.extent.height;
			image->layers = info.arrayLayers;
			image->levels = info.mipLevels;
			image->usage = info.usage;
			image->layer_bytes = mali_surface_levels(info.extent.width, info.extent.height, format->bytes, MALI_VK_ROW_ALIGN, info.mipLevels, image->level);
			image->row_pitch = image->level[0].pitch;
			image->bytes = image->layer_bytes * info.arrayLayers;
			image->bytes = (image->bytes + MALI_VK_IMAGE_ALIGN - 1U) & ~(uint64_t)(MALI_VK_IMAGE_ALIGN - 1U);
		}
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_IMAGE, created, image, image_destroy, error);
	return 0;
}

/* Destroys an image: its hold on its memory goes. */
static int
image_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_image *image;

	(void)session;
	image = payload;
	mali_vk_object_release(image->memory);
	kern_free(image);
	return 0;
}

/* vkGet{Buffer,Image}MemoryRequirements: [device][object][present] -> [present][requirements]. */
static int
requirements(
	struct mali_vk_session *session,
	bool image,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkMemoryRequirements record;
	struct mali_vk_buffer *buffer;
	struct mali_vk_image *picture;
	uint64_t named;

	/* The object. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Its bytes and alignment; both types hold either. */
	kern_memset(&record, 0, sizeof(record));
	record.memoryTypeBits = (1U << MALI_VK_MEMORY_TYPES) - 1U;
	if (image) {
		picture = mali_vk_payload(session, I915_VK_OBJ_IMAGE, named);
		if (picture != NULL) {
			record.size = picture->bytes;
			record.alignment = MALI_VK_IMAGE_ALIGN;
		}
	} else {
		buffer = mali_vk_payload(session, I915_VK_OBJ_BUFFER, named);
		if (buffer != NULL) {
			record.size = (buffer->size + MALI_VK_BUFFER_ALIGN - 1U) & ~(uint64_t)(MALI_VK_BUFFER_ALIGN - 1U);
			record.alignment = MALI_VK_BUFFER_ALIGN;
		}
	}

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkMemoryRequirements(reply, &record);
	return 0;
}

/* vkBind{Buffer,Image}Memory: [device][object][memory][offset] -> [result]. */
static int
bind(
	struct mali_vk_session *session,
	bool image,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_object *memory_object;
	struct mali_vk_memory *memory;
	struct mali_vk_buffer *buffer;
	struct mali_vk_image *picture;
	uint64_t named;
	uint64_t memory_identity;
	uint64_t offset;
	uint64_t bytes;
	int error;

	/* The object, the memory and the offset. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	memory_identity = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The object and the memory. */
	buffer = NULL;
	picture = NULL;
	if (image) {
		picture = mali_vk_payload(session, I915_VK_OBJ_IMAGE, named);
	} else {
		buffer = mali_vk_payload(session, I915_VK_OBJ_BUFFER, named);
	}

	memory_object = mali_vk_object_find(session, I915_VK_OBJ_MEMORY, memory_identity);
	error = 0;
	if ((picture == NULL && buffer == NULL) || memory_object == NULL)
		error = EINVAL;

	/* A range inside the memory, bound once. */
	if (error == 0) {
		memory = memory_object->payload;
		bytes = picture != NULL ? picture->bytes : buffer->size;
		if (offset > memory->bytes || bytes > memory->bytes - offset)
			error = EINVAL;
		if ((picture != NULL && picture->memory != NULL) || (buffer != NULL && buffer->memory != NULL))
			error = EINVAL;
	}

	/* The memory is held by the object. */
	if (error == 0)
		error = mali_vk_object_retain(memory_object);
	if (error == 0 && picture != NULL) {
		picture->memory = memory_object;
		picture->offset = offset;
	} else if (error == 0) {
		buffer->memory = memory_object;
		buffer->offset = offset;
	}

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, mali_vk_result(error));
	return 0;
}

/*
 * vkGetImageSubresourceLayout: a level's offset (layer after layer, the
 * levels of a layer one after another), its pitch and its bytes; the
 * array pitch is a layer's.  A level or a layer outside the image gives
 * level 0 or layer 0.
 */
static int
subresource_layout(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	struct mali_vk_image *image;
	uint64_t named;
	uint64_t present;
	uint32_t level;

	/* The image and the subresource. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	kern_memset(&subresource, 0, sizeof(subresource));
	if (present != 0U)
		i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The layout. */
	kern_memset(&layout, 0, sizeof(layout));
	image = mali_vk_payload(session, I915_VK_OBJ_IMAGE, named);
	if (image != NULL) {
		level = 0;
		if (subresource.mipLevel < image->levels)
			level = subresource.mipLevel;
		layout.offset = image->level[level].offset;
		if (subresource.arrayLayer < image->layers)
			layout.offset += (uint64_t)subresource.arrayLayer * image->layer_bytes;
		layout.size = image->level[level].bytes;
		layout.rowPitch = image->level[level].pitch;
		layout.arrayPitch = image->layer_bytes;
		layout.depthPitch = image->level[level].bytes;
	}

	/* Succeeded: the reply. */
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkSubresourceLayout(reply, &layout);
	return 0;
}

/*
 * vkCreateImageView: a 2D, 2D array, cube or cube array view of layers
 * and levels of the image, its format the image's or one of the same
 * texel size.
 */
static int
create_view(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageViewCreateInfo info;
	struct mali_vk_image_view *view;
	struct mali_vk_object *image;
	const struct mali_vk_format *format;
	const struct mali_vk_image *bound;
	uint64_t present;
	uint64_t created;
	uint32_t layers;
	uint32_t levels;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkImageViewCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A known image and format of the same texel size. */
	error = 0;
	view = NULL;
	image = mali_vk_object_find(session, I915_VK_OBJ_IMAGE, mali_vk_handle(info.image));
	format = mali_vk_format_find(info.format);
	if (image == NULL || format == NULL) {
		error = ENOTSUP;
	} else if (format->bytes != ((struct mali_vk_image *)image->payload)->format->bytes) {
		error = ENOTSUP;
	}

	/* The levels: the rest of the image's for VK_REMAINING_MIP_LEVELS, inside it. */
	levels = 0U;
	if (error == 0) {
		bound = image->payload;
		levels = info.subresourceRange.levelCount;
		if (levels == VK_REMAINING_MIP_LEVELS && info.subresourceRange.baseMipLevel < bound->levels)
			levels = bound->levels - info.subresourceRange.baseMipLevel;
		if (levels == 0U || info.subresourceRange.baseMipLevel >= bound->levels || levels > bound->levels - info.subresourceRange.baseMipLevel)
			error = EINVAL;
	}

	/* The layers: the rest of the image's for VK_REMAINING_ARRAY_LAYERS, inside it, six a cube. */
	layers = 0U;
	if (error == 0) {
		bound = image->payload;
		layers = info.subresourceRange.layerCount;
		if (layers == VK_REMAINING_ARRAY_LAYERS && info.subresourceRange.baseArrayLayer < bound->layers)
			layers = bound->layers - info.subresourceRange.baseArrayLayer;
		if (layers == 0U || info.subresourceRange.baseArrayLayer >= bound->layers || layers > bound->layers - info.subresourceRange.baseArrayLayer) {
			error = EINVAL;
		} else if (info.viewType == VK_IMAGE_VIEW_TYPE_2D && layers != 1U) {
			error = EINVAL;
		} else if (info.viewType == VK_IMAGE_VIEW_TYPE_CUBE && layers != 6U) {
			error = EINVAL;
		} else if (info.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && layers % 6U != 0U) {
			error = EINVAL;
		} else if (info.viewType != VK_IMAGE_VIEW_TYPE_2D && info.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
			   info.viewType != VK_IMAGE_VIEW_TYPE_CUBE && info.viewType != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
			/* A 1D or 3D view has no image of its kind here. */
			error = ENOTSUP;
		}
	}

	/* The view holds the image. */
	if (error == 0) {
		view = kern_calloc(1, sizeof(*view));
		if (view == NULL)
			error = ENOMEM;
	}

	if (error == 0) {
		error = mali_vk_object_retain(image);
		if (error != 0) {
			kern_free(view);
			view = NULL;
		}
	}

	/*
	 * The swizzle of the components, three bits a channel (R 0, G 1, B 2,
	 * A 3, zero 4, one 5).
	 *
	 * Values referenced:
	 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Channel").
	 */
	if (view != NULL) {
		view->image = image;
		view->format = format;
		view->base_layer = info.subresourceRange.baseArrayLayer;
		view->view_type = info.viewType;
		view->layer_count = layers;
		view->base_level = info.subresourceRange.baseMipLevel;
		view->level_count = levels;
		view->swizzle = swizzle_channel(info.components.r, 0U);
		view->swizzle |= swizzle_channel(info.components.g, 1U) << 3;
		view->swizzle |= swizzle_channel(info.components.b, 2U) << 6;
		view->swizzle |= swizzle_channel(info.components.a, 3U) << 9;
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_IMAGE_VIEW, created, view, view_destroy, error);
	return 0;
}

/* Destroys a view: its hold on its image goes. */
static int
view_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_image_view *view;

	(void)session;
	view = payload;
	mali_vk_object_release(view->image);
	kern_free(view);
	return 0;
}

/* vkCreateSampler: the sampler descriptor's words. */
static int
create_sampler(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkSamplerCreateInfo info;
	struct mali_vk_sampler *sampler;
	uint64_t present;
	uint64_t created;
	uint32_t word;
	uint32_t min_lod;
	uint32_t max_lod;
	uint32_t bias;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkSamplerCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The first word: the type, the wrap modes, the filters, no mipmaps until the levels' range says otherwise. */
	sampler = kern_calloc(1, sizeof(*sampler));
	if (sampler != NULL) {
		word = SAMPLER_TYPE;
		word |= wrap_mode(info.addressModeW) << SAMPLER_WRAP_R_SHIFT;
		word |= wrap_mode(info.addressModeV) << SAMPLER_WRAP_T_SHIFT;
		word |= wrap_mode(info.addressModeU) << SAMPLER_WRAP_S_SHIFT;
		word |= SAMPLER_ROUND_EVEN | SAMPLER_SEAMLESS | SAMPLER_CLAMP_ARRAY | SAMPLER_MIPMAP_NONE;
		if (info.unnormalizedCoordinates == VK_FALSE)
			word |= SAMPLER_NORMALIZED;
		if (info.minFilter == VK_FILTER_NEAREST)
			word |= SAMPLER_MIN_NEAREST;
		if (info.magFilter == VK_FILTER_NEAREST)
			word |= SAMPLER_MAG_NEAREST;
		sampler->words[0] = word;

		/* The compare function, one sample of anisotropy, a transparent black border. */
		sampler->words[1] = 0;
		if (info.compareEnable != VK_FALSE)
			sampler->words[1] |= mali_desc_sampler_compare(info.compareOp);
		sampler->words[2] = 0;

		/* The levels' range, bias and mipmap mode as float bits; a range of level 0 without a bias keeps no mipmaps. */
		kern_memcpy(&min_lod, &info.minLod, sizeof(uint32_t));
		kern_memcpy(&max_lod, &info.maxLod, sizeof(uint32_t));
		kern_memcpy(&bias, &info.mipLodBias, sizeof(uint32_t));
		(void)mali_surface_sampler_lod(sampler->words, info.mipmapMode, min_lod, max_lod, bias);
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_SAMPLER, created, sampler, mali_vk_free_payload, 0);
	return 0;
}

/* Gives the hardware's wrap mode of a Vulkan address mode. */
static uint32_t
wrap_mode(
	uint32_t mode)
{
	/* One case a mode; clamp to edge for any other. */
	switch (mode) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		return WRAP_REPEAT;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		return WRAP_MIRRORED;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		return WRAP_CLAMP_BORDER;
	default:
		return WRAP_CLAMP_EDGE;
	}
}

/* vkCreateFence, vkCreateSemaphore: the create form with [sType][chain][flags]. */
static int
create_sync(
	struct mali_vk_session *session,
	bool fence,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_fence *payload;
	uint64_t present;
	uint64_t created;
	uint32_t flags;

	/* The device, the info and the tail. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	flags = 0;
	if (present != 0U) {
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		flags = drv_i915_wire_read_u32(reader);
	}
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The payload; a fence may start signaled. */
	payload = kern_calloc(1, sizeof(*payload));
	if (payload != NULL && fence && (flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0U)
		payload->signaled = true;

	/* Succeeded: the reply publishes it. */
	if (fence) {
		mali_vk_create_reply(session, reply, I915_VK_OBJ_FENCE, created, payload, mali_vk_free_payload, 0);
	} else {
		mali_vk_create_reply(session, reply, I915_VK_OBJ_SEMAPHORE, created, payload, mali_vk_free_payload, 0);
	}
	return 0;
}

/* vkResetFences: [device][count][count][fences] -> [result]. */
static int
reset_fences(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_fence *fence;
	uint64_t count;
	uint64_t index;

	/* The device and the counts. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > 4096U)
		return EINVAL;

	/* Each fence unsignaled. */
	for (index = 0; index < count; index++) {
		fence = mali_vk_payload(session, I915_VK_OBJ_FENCE, drv_i915_wire_read_u64(reader));
		if (fence != NULL)
			fence->signaled = false;
	}

	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* vkGetFenceStatus: [device][fence] -> [result]. */
static int
fence_status(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_fence *fence;
	uint64_t named;

	/* The fence. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Signaled or not yet. */
	fence = mali_vk_payload(session, I915_VK_OBJ_FENCE, named);
	if (fence != NULL && fence->signaled) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	} else {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_NOT_READY);
	}

	/* Succeeded: the result. */
	return 0;
}

/* Gives a view's channel code for one component: the identity's, or the one it names. */
static uint32_t
swizzle_channel(
	uint32_t component,
	uint32_t identity)
{
	/* One case a component swizzle. */
	switch (component) {
	case VK_COMPONENT_SWIZZLE_R:
		return 0U;
	case VK_COMPONENT_SWIZZLE_G:
		return 1U;
	case VK_COMPONENT_SWIZZLE_B:
		return 2U;
	case VK_COMPONENT_SWIZZLE_A:
		return 3U;
	case VK_COMPONENT_SWIZZLE_ZERO:
		return 4U;
	case VK_COMPONENT_SWIZZLE_ONE:
		return 5U;
	default:
		return identity;
	}
}
