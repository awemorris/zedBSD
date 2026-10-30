/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Buffer and raster colour-image requirements remain immutable through memory, resource and prepared-job retirement. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-memory.h"
#include "drivers/gpu/bcm2711/vk/vulkan-resource.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"

#if CONFIG_DRIVER_BCM2711_VIDEO
#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#endif

/* Existing generated record decoding is device-independent Zlib code, without a Gen12 resource implementation. */
#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The queried native subset has one allocation type, one level/layer/sample and 4096-square colour images. */
#define VULKAN_RESOURCE_BYTES (256ULL << 20)
#define VULKAN_BUFFER_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
#define VULKAN_IMAGE_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)

/* A depth/stencil image is only ever an attachment, transient or not; the tile buffer loads and stores it. */
#define VULKAN_DEPTH_USAGE (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)

/* The create flags a two-plane picture may carry so its planes can be viewed in their own formats. */
#define VULKAN_IMAGE_PLANE_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)

static int create_buffer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_image(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int publish_resource(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct bcm2711_vulkan_resource *description, uint64_t device_id, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int release_resource(struct bcm2711_vulkan_session *session, void *payload);
static int destroy_resource(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int memory_requirements(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int bind_memory(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_layout(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static struct bcm2711_vulkan_resource *find_resource(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t device_id, uint64_t identity);
static int external_declaration(struct i915_wire_reader *reader, uint32_t expected);
static int sharing_mode(VkSharingMode mode, uint32_t count, const uint32_t *families);
static void creation_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes native storage description, requirements, exact memory binding and colour-image layout commands.
 */
int
bcm2711_vulkan_resource_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Only implemented typed operations consume a command; other resources remain another router's responsibility. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_BUFFER:
	case GPU_OP_CREATE_IMAGE:
	case GPU_OP_DESTROY_BUFFER:
	case GPU_OP_DESTROY_IMAGE:
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
	case GPU_OP_BIND_BUFFER_MEMORY:
	case GPU_OP_BIND_IMAGE_MEMORY:
	case GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT:
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Ordinary void destruction still permits the client's echoed-opcode flag. */
	if (requested > 1)
		return EINVAL;
	if (opcode != GPU_OP_DESTROY_BUFFER &&
	    opcode != GPU_OP_DESTROY_IMAGE &&
	    requested != 1)
		return EINVAL;

	/* Resource kind always follows the opcode, never an untyped identity lookup. */
	switch (opcode) {
	case GPU_OP_CREATE_BUFFER:
		error = create_buffer(session, reader, reply);
		break;
	case GPU_OP_CREATE_IMAGE:
		error = create_image(session, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER:
		error = destroy_resource(session, I915_VK_OBJ_BUFFER, reader);
		break;
	case GPU_OP_DESTROY_IMAGE:
		error = destroy_resource(session, I915_VK_OBJ_IMAGE, reader);
		break;
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
		error = memory_requirements(session, I915_VK_OBJ_BUFFER, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
		error = memory_requirements(session, I915_VK_OBJ_IMAGE, reader, reply);
		break;
	case GPU_OP_BIND_BUFFER_MEMORY:
		error = bind_memory(session, I915_VK_OBJ_BUFFER, reader, reply);
		break;
	case GPU_OP_BIND_IMAGE_MEMORY:
		error = bind_memory(session, I915_VK_OBJ_IMAGE, reader, reply);
		break;
	default:
		error = image_layout(session, reader, reply);
		break;
	}

	/* Native framing or ownership failure stops this immutable submission. */
	if (error != 0)
		return error;

	/* Succeeded: one exact typed resource operation completed. */
	return 0;
}

/*
 * Resolves one bounded resource span to its retained native VA view and coherent CPU alias.
 */
int
bcm2711_vulkan_resource_backing(
	struct bcm2711_vulkan_resource *resource,
	uint64_t offset,
	uint64_t bytes,
	struct bcm2711_v3d_view **view,
	uint32_t *address,
	void **cpu)
{
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_buffer *buffer;
	uint64_t limit;
	uint64_t begin;
	uint64_t native;

	/* An unbound description grants neither a native address nor a CPU alias. */
	*view = NULL;
	*address = 0;
	*cpu = NULL;
	if (resource == NULL ||
	    resource->memory == NULL ||
	    bytes == 0)
		return EINVAL;

	/* A mip-mapped image's levels lie after level 0, inside its whole storage. */
	limit = resource->bytes;
	if (resource->storage_bytes > limit)
		limit = resource->storage_bytes;
	if (offset > limit || bytes > limit - offset)
		return EINVAL;
	memory = resource->memory->payload;
	if (memory->view == NULL || memory->view->quarantined)
		return EIO;

	/* The binding was bounded at publication; recheck the exact backing before exposing a draw/transfer span. */
	begin = resource->offset + offset;
	if (begin < resource->offset ||
	    begin > memory->bytes ||
	    bytes > memory->bytes - begin)
		return EINVAL;
	buffer = memory->view->buffer;
	if (!buffer->uncached ||
	    buffer->address == NULL ||
	    begin > buffer->bytes ||
	    bytes > buffer->bytes - begin)
		return EIO;
	native = (uint64_t)memory->view->address + begin;
	if (native >= 0x100000000ULL || bytes > 0x100000000ULL - native)
		return EOVERFLOW;

	/* Returned pointers are borrowed; prepared jobs must retain the resource or view before leaving the controller mutex. */
	*view = memory->view;
	*address = (uint32_t)native;
	*cpu = (uint8_t *)buffer->address + begin;

	/* Succeeded: the entire requested span belongs to the retained immutable binding. */
	return 0;
}

/*
 * Describes one mip level of an image: level 0 is the image's own raster;
 * each smaller level halves the size above it (to no less than one texel)
 * and starts where the one above ends, at a 64-byte multiple pitch.
 */
int
bcm2711_vulkan_image_level(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level,
	struct bcm2711_vulkan_image_level *result)
{
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t index;

	/* Only the image's own levels exist; a one-level image counts as one. */
	if (image == NULL || result == NULL)
		return EINVAL;
	if (level != 0 && level >= image->levels)
		return EINVAL;

	/* Level 0 is the image's raster. */
	width = image->width;
	height = image->height;
	pitch = image->pitch;
	offset = 0;

	/* Each smaller level starts after the one above it. */
	for (index = 1; index <= level; index++) {
		offset += (uint64_t)pitch * height;
		width >>= 1;
		if (width == 0)
			width = 1;
		height >>= 1;
		if (height == 0)
			height = 1;
		pitch = (width * 4U + 63U) & ~63U;
	}

	/* The level's rows. */
	result->offset = offset;
	result->bytes = (uint64_t)pitch * height;
	result->width = width;
	result->height = height;
	result->pitch = pitch;

	/* Succeeded: the caller has the level's place and size. */
	return 0;
}

/*
 * Fills view with one mip level of an image as a one-level image of its
 * own: the level's raster at the binding offset moved to it, borrowing the
 * image's memory and device (the caller keeps the image retained while it
 * uses the view, and never publishes or releases it).
 */
int
bcm2711_vulkan_image_level_view(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level,
	struct bcm2711_vulkan_resource *view)
{
	struct bcm2711_vulkan_image_level geometry;
	int error;

	/* The level's place and size. */
	error = bcm2711_vulkan_image_level(image, level, &geometry);
	if (error != 0)
		return error;

	/* The view is the image moved to the level's rows, with only that level. */
	*view = *image;
	view->offset = image->offset + geometry.offset;
	view->bytes = geometry.bytes;
	view->required_bytes = geometry.bytes;
	view->width = geometry.width;
	view->height = geometry.height;
	view->pitch = geometry.pitch;
	view->levels = 1;
	view->storage_bytes = 0;
	view->layout = bcm2711_vulkan_image_level_layout(image, level);

	/* Succeeded: the view addresses the level's raster alone. */
	return 0;
}

/* Reports the current layout of one mip level of an image. */
VkImageLayout
bcm2711_vulkan_image_level_layout(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level)
{
	/* Level 0's layout is the image's own. */
	if (level == 0 || level >= BCM2711_VULKAN_IMAGE_LEVELS)
		return image->layout;

	/* Succeeded: a smaller level's own layout. */
	return image->level_layouts[level];
}

/* Records the layout one mip level of an image moved to. */
void
bcm2711_vulkan_image_set_level_layout(
	struct bcm2711_vulkan_resource *image,
	uint32_t level,
	VkImageLayout layout)
{
	/* Level 0's layout is the image's own. */
	if (level == 0) {
		image->layout = layout;
	} else if (level < BCM2711_VULKAN_IMAGE_LEVELS) {
		image->level_layouts[level] = layout;
	}

	/* Succeeded: following operations see the level's layout. */
	return;
}

/* Creates one native buffer description from the client's full standard record. */
static int
create_buffer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct bcm2711_vulkan_resource description;
	uint64_t device_id;
	uint64_t present;
	uint32_t allowed;
	int error;

	/* Decode the required input using the existing bounded command arena. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 1 || reader->error != 0)
		return EINVAL;
	error = external_declaration(reader, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
	if (error != 0)
		return error;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return reader->error;
	if (info.sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
	    info.flags != 0 ||
	    info.size == 0 ||
	    info.size > VULKAN_RESOURCE_BYTES ||
	    info.usage == 0)
		return ENOTSUP;
	allowed = VULKAN_BUFFER_USAGE;
#if CONFIG_DRIVER_BCM2711_VIDEO
	/* A bitstream buffer is read by the CPU into the video decoder's staging. */
	allowed |= BCM2711_VIDEO_BUFFER_USAGE_DECODE_SRC;
#endif
	if ((info.usage & ~allowed) != 0)
		return ENOTSUP;
	error = sharing_mode(info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices);
	if (error != 0)
		return error;

	/* Buffer allocation requirements use a complete 64-byte-aligned extent, without committing storage yet. */
	kern_memset(&description, 0, sizeof(description));
	description.bytes = info.size;
	description.required_bytes = (info.size + 63U) & ~63ULL;
	description.alignment = 64;
	description.usage = info.usage;
	error = publish_resource(session, I915_VK_OBJ_BUFFER, &description, device_id, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the typed buffer owns an immutable unbound storage description. */
	return 0;
}

/* Creates one single-layer raster colour image, with its mip levels, with exact native row pitch. */
static int
create_image(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct bcm2711_vulkan_resource description;
	struct bcm2711_vulkan_image_level geometry;
	uint64_t device_id;
	uint64_t present;
	uint32_t largest;
	uint32_t level;
#if CONFIG_DRIVER_BCM2711_VIDEO
	uint64_t chroma;
#endif
	bool nv12;
	bool depth;
	int error;

	/* The codec accepts the client's stripped external declaration; native storage capabilities remain separate. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 1 || reader->error != 0)
		return EINVAL;
	error = external_declaration(reader, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
	if (error != 0)
		return error;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkImageCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return reader->error;
	if (info.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
	    (info.flags & ~VULKAN_IMAGE_PLANE_FLAGS) != 0 ||
	    info.imageType != VK_IMAGE_TYPE_2D ||
	    info.extent.width == 0 ||
	    info.extent.width > 4096 ||
	    info.extent.height == 0 ||
	    info.extent.height > 4096 ||
	    info.extent.depth != 1 ||
	    info.mipLevels == 0 ||
	    info.mipLevels > BCM2711_VULKAN_IMAGE_LEVELS ||
	    info.arrayLayers != 1 ||
	    info.samples != VK_SAMPLE_COUNT_1_BIT)
		return ENOTSUP;
	nv12 = false;
#if CONFIG_DRIVER_BCM2711_VIDEO
	/*
	 * An NV12 picture decodes, serves as a reference, is read back, and is
	 * sampled through views of its planes.
	 */
	if (info.format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
		nv12 = true;
		if (info.usage == 0 ||
		    (info.usage & ~(BCM2711_VIDEO_USAGE_DECODE_DST | BCM2711_VIDEO_USAGE_DECODE_DPB | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) != 0)
			return ENOTSUP;
	}
#endif

	/*
	 * A depth/stencil image (ws141-p017) is an optimally tiled attachment
	 * with the tile buffer's 32-bit D24S8 or X8_D24 pixels, kept in the
	 * same raster form as a colour image.
	 */
	depth = false;
	if (info.format == VK_FORMAT_D24_UNORM_S8_UINT || info.format == VK_FORMAT_X8_D24_UNORM_PACK32) {
		depth = true;
		if (info.flags != 0 ||
		    info.tiling != VK_IMAGE_TILING_OPTIMAL ||
		    (info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 ||
		    (info.usage & ~VULKAN_DEPTH_USAGE) != 0)
			return ENOTSUP;
	}

	/* Only an NV12 picture's plane views reinterpret the image's format. */
	if (!nv12 && info.flags != 0)
		return ENOTSUP;
	if (!nv12 && !depth && info.format != VK_FORMAT_R8G8B8A8_UNORM && info.format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;
	if (info.tiling != VK_IMAGE_TILING_LINEAR && info.tiling != VK_IMAGE_TILING_OPTIMAL)
		return ENOTSUP;
	if (!nv12 && !depth && (info.usage == 0 || (info.usage & ~VULKAN_IMAGE_USAGE) != 0))
		return ENOTSUP;
	if (info.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
	    (info.initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED || info.tiling != VK_IMAGE_TILING_LINEAR))
		return ENOTSUP;
	error = sharing_mode(info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices);
	if (error != 0)
		return error;

	/* Only an optimally tiled colour image has mip levels, no more than its size halves to one texel. */
	if (info.mipLevels != 1) {
		if (nv12 || depth || info.tiling != VK_IMAGE_TILING_OPTIMAL)
			return ENOTSUP;
		largest = info.extent.width;
		if (info.extent.height > largest)
			largest = info.extent.height;
		if ((largest >> (info.mipLevels - 1U)) == 0)
			return EINVAL;
	}

	/* Optimal images use the same native raster backing; Vulkan does not require exposing their CPU subresource layout. */
	kern_memset(&description, 0, sizeof(description));
	description.width = info.extent.width;
	description.height = info.extent.height;
	description.pitch = (info.extent.width * 4U + 63U) & ~63U;
	description.bytes = (uint64_t)description.pitch * info.extent.height;
#if CONFIG_DRIVER_BCM2711_VIDEO
	/* NV12 rows are the decoder's: 32-byte pitch, luma then interleaved chroma (vulkan-video.c). */
	if (nv12)
		bcm2711_vulkan_video_nv12_layout(info.extent.width, info.extent.height, &description.pitch, &chroma, &description.bytes);
#endif
	description.required_bytes = description.bytes;
	description.alignment = 64;
	description.usage = info.usage;
	description.format = info.format;
	description.tiling = info.tiling;
	description.layout = info.initialLayout;

	/* The smaller levels follow level 0, each starting where the one above ends; all start in the initial layout. */
	description.levels = info.mipLevels;
	description.storage_bytes = description.bytes;
	for (level = 1; level < info.mipLevels; level++) {
		error = bcm2711_vulkan_image_level(&description, level, &geometry);
		if (error != 0)
			return error;
		description.storage_bytes = geometry.offset + geometry.bytes;
		description.level_layouts[level] = info.initialLayout;
	}
	description.required_bytes = description.storage_bytes;
	error = publish_resource(session, I915_VK_OBJ_IMAGE, &description, device_id, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the colour image has one immutable native layout and no physical ownership before binding. */
	return 0;
}

/* Publishes a complete copied resource description after acquiring its logical-device dependency. */
static int
publish_resource(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct bcm2711_vulkan_resource *description,
	uint64_t device_id,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_resource *resource;
	uint64_t allocator;
	uint64_t present;
	uint64_t identity;
	int error;
	int retired;

	/* Creation completes the standard null allocator and exact guest-chosen typed output identity. */
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    allocator != 0 ||
	    present != 1 ||
	    identity == 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (object != NULL)
		return EEXIST;
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL) {
		creation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* The copied description contains no generated-arena pointers or guest application addresses. */
	*resource = *description;
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* An independent device edge exists before the resource can enter its typed registry. */
	resource->device = device;
	error = bcm2711_vulkan_object_publish(session, kind, identity, resource, release_resource, &object);
	if (error != 0) {
		retired = release_resource(session, resource);
		if (retired != 0)
			return retired;
		if (error == ENOMEM || error == ENOSPC) {
			creation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* A framing refusal is distinct from a structured ordinary allocation failure. */
		return error;
	}

	/* Acknowledge only the fully published typed description. */
	creation_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: the namespace owns one unbound native resource description. */
	return 0;
}

/* Releases memory and device dependency edges after the final resource or prepared-job owner retires. */
static int
release_resource(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_resource *resource;
	int error;
	int parent_error;

	/* Bound memory remains alive until this resource's final dependent owner releases it. */
	(void)session;
	resource = payload;
	error = bcm2711_vulkan_object_release(resource->memory);
	parent_error = bcm2711_vulkan_object_release(resource->device);
	kern_free(resource);

	/* Native memory retirement errors remain visible after logical metadata retirement. */
	if (error != 0)
		return error;
	if (parent_error != 0)
		return parent_error;

	/* Succeeded: no native allocation or root is owned by this retired resource description. */
	return 0;
}

/* Withdraws only the selected resource identity while retained dependent owners keep its binding alive. */
static int
destroy_resource(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_resource *resource;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Ordinary destruction uses the standard null allocator and same-device typed identity. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	if (identity == 0)
		return 0;
	resource = find_resource(session, kind, device_id, identity);
	if (resource == NULL)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: only the registry's resource ownership edge retired. */
	return 0;
}

/* Returns the exact immutable size/alignment and the one native coherent memory-type bit. */
static int
memory_requirements(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_resource *resource;
	VkMemoryRequirements requirements;
	uint64_t device_id;
	uint64_t identity;
	uint64_t present;

	/* The device and typed resource must resolve before any native requirements can be reported. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	resource = find_resource(session, kind, device_id, identity);
	if (resource == NULL)
		return EINVAL;

	/* The public record matches the exact native extent required by memory binding. */
	kern_memset(&requirements, 0, sizeof(requirements));
	requirements.size = resource->required_bytes;
	requirements.alignment = resource->alignment;
	requirements.memoryTypeBits = 1;
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkMemoryRequirements(reply, &requirements);

	/* Succeeded: the actual client decoder can select native coherent type zero without guessing. */
	return 0;
}

/* Binds one complete native memory interval while acquiring an independent allocation dependency. */
static int
bind_memory(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_memory *memory;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocation;
	uint64_t offset;
	int error;

	/* The client's exact memory offset follows two typed identities on the selected device. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocation = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	resource = find_resource(session, kind, device_id, identity);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, allocation);
	if (resource == NULL ||
	    object == NULL ||
	    resource->memory != NULL)
		return EINVAL;
	memory = object->payload;
	if (memory->device != resource->device)
		return EINVAL;

	/* Unsupported, misaligned or partial intervals never mutate the existing unbound resource. */
	if (memory->view == NULL ||
	    memory->view->quarantined ||
	    (offset & (resource->alignment - 1U)) != 0 ||
	    offset > memory->bytes ||
	    resource->required_bytes > memory->bytes - offset) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY);
		return 0;
	}

	/* The binding owns the actual memory object independently of its published allocation identity. */
	error = bcm2711_vulkan_object_retain(object);
	if (error != 0)
		return error;
	resource->memory = object;
	resource->offset = offset;
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: the resource's entire storage interval is retained until its final native owner retires. */
	return 0;
}

/* Returns the single colour subresource's raster layout only for an actual linear image. */
static int
image_layout(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_resource *resource;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	uint64_t device_id;
	uint64_t identity;
	uint64_t input;
	uint64_t output;
#if CONFIG_DRIVER_BCM2711_VIDEO
	uint64_t chroma;
	uint64_t bytes;
	uint32_t pitch;
#endif

	/* Both generated input and output pointers must describe the one supported complete colour subresource. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	input = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || input != 1)
		return EINVAL;
	kern_memset(&subresource, 0, sizeof(subresource));
	i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);
	output = drv_i915_wire_read_u64(reader);
#if CONFIG_DRIVER_BCM2711_VIDEO
	/* The plane aspects of an NV12 picture are checked with its format below. */
	if (reader->error != 0 ||
	    output != 1 ||
	    subresource.mipLevel != 0 ||
	    subresource.arrayLayer != 0)
		return EINVAL;
	resource = find_resource(session, I915_VK_OBJ_IMAGE, device_id, identity);
	if (resource == NULL)
		return ENOTSUP;
#else
	if (reader->error != 0 ||
	    output != 1 ||
	    subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
	    subresource.mipLevel != 0 ||
	    subresource.arrayLayer != 0)
		return EINVAL;
	resource = find_resource(session, I915_VK_OBJ_IMAGE, device_id, identity);
	if (resource == NULL || resource->tiling != VK_IMAGE_TILING_LINEAR)
		return ENOTSUP;
#endif

	/* The first subresource starts at zero relative to the binding, never at a host pointer or GPU VA. */
	kern_memset(&layout, 0, sizeof(layout));
	layout.size = resource->bytes;
	layout.rowPitch = resource->pitch;
	layout.arrayPitch = resource->bytes;
	layout.depthPitch = resource->bytes;
#if CONFIG_DRIVER_BCM2711_VIDEO
	/*
	 * An NV12 picture's planes are answered for either tiling (the private
	 * promise libvulkan's README calls N3): its rows are linear.
	 */
	if (resource->format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
		bcm2711_vulkan_video_nv12_layout(resource->width, resource->height, &pitch, &chroma, &bytes);
		if (subresource.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT) {
			layout.offset = 0;
			layout.size = chroma;
		} else if (subresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
			layout.offset = chroma;
			layout.size = bytes - chroma;
		} else {
			return EINVAL;
		}
		layout.rowPitch = pitch;
		drv_i915_wire_reply_u64(reply, 1);
		i915_vkc_enc_VkSubresourceLayout(reply, &layout);
		return 0;
	}
	if (subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
		return EINVAL;
	if (resource->tiling != VK_IMAGE_TILING_LINEAR)
		return ENOTSUP;
#endif
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkSubresourceLayout(reply, &layout);

	/* Succeeded: the client receives exact immutable raster pitch and extent. */
	return 0;
}

/* Finds a borrowed typed resource whose retained parent is the exact live selected logical device. */
static struct bcm2711_vulkan_resource *
find_resource(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t device_id,
	uint64_t identity)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_resource *resource;

	/* Both lookups stay inside the same open's typed registry under the controller mutex. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (device == NULL || object == NULL)
		return NULL;
	resource = object->payload;
	if (resource->device != device)
		return NULL;

	/* Succeeded: the caller borrows one immutable same-device resource description. */
	return resource;
}

/* Checks the codec's optional external-memory declaration without consuming the complete standard record. */
static int
external_declaration(
	struct i915_wire_reader *reader,
	uint32_t expected)
{
	struct i915_wire_reader checked;
	uint64_t present;
	uint64_t next;
	uint32_t type;
	uint32_t handles;

	/* A copied immutable cursor lets the ordinary generated decoder consume the same record afterward. */
	checked = *reader;
	(void)drv_i915_wire_read_u32(&checked);
	present = drv_i915_wire_read_u64(&checked);
	if (checked.error != 0 || present > 1)
		return EINVAL;
	if (present == 0)
		return 0;

	/* The complete native declaration names only opaque or private WSI sharing, with no unsupported chained extension. */
	type = drv_i915_wire_read_u32(&checked);
	next = drv_i915_wire_read_u64(&checked);
	handles = drv_i915_wire_read_u32(&checked);
	if (checked.error != 0 ||
	    type != expected ||
	    next != 0)
		return ENOTSUP;
	if (handles == 0 || (handles & ~0x201U) != 0)
		return ENOTSUP;

	/* Succeeded: the generated record's skipped extension has no unimplemented native semantics. */
	return 0;
}

/* Accepts only the native graphics queue family, including exact single-family concurrent declarations. */
static int
sharing_mode(
	VkSharingMode mode,
	uint32_t count,
	const uint32_t *families)
{
	/* Exclusive mode does not consume the standard's ignored family-index fields. */
	if (mode == VK_SHARING_MODE_EXCLUSIVE)
		return 0;
	if (mode != VK_SHARING_MODE_CONCURRENT ||
	    count != 1 ||
	    families == NULL ||
	    families[0] != 0)
		return ENOTSUP;

	/* Succeeded: one queue family owns every supported access to this native description. */
	return 0;
}

/* Writes the ordinary creation result and exact typed output identity. */
static void
creation_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* A failed ordinary creation retains no local output identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the reply carries the selected resource status and identity. */
	return;
}
