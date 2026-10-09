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

#include "drivers/gpu/bcm2711/vulkan-memory.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"

/* Existing generated record decoding is device-independent Zlib code, without a Gen12 resource implementation. */
#include "drivers/gpu/i915/render/vulkan-codec.inc"

/* The queried native subset has one allocation type, one level/layer/sample and 4096-square colour images. */
#define VULKAN_RESOURCE_BYTES (256ULL << 20)
#define VULKAN_BUFFER_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
#define VULKAN_IMAGE_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)

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
	if (opcode != GPU_OP_DESTROY_BUFFER && opcode != GPU_OP_DESTROY_IMAGE && requested != 1)
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
	uint64_t begin;
	uint64_t native;

	/* An unbound description grants neither a native address nor a CPU alias. */
	*view = NULL;
	*address = 0;
	*cpu = NULL;
	if (resource == NULL || resource->memory == NULL || bytes == 0)
		return EINVAL;
	if (offset > resource->bytes || bytes > resource->bytes - offset)
		return EINVAL;
	memory = resource->memory->payload;
	if (memory->view == NULL || memory->view->quarantined)
		return EIO;

	/* The binding was bounded at publication; recheck the exact backing before exposing a draw/transfer span. */
	begin = resource->offset + offset;
	if (begin < resource->offset || begin > memory->bytes || bytes > memory->bytes - begin)
		return EINVAL;
	buffer = memory->view->buffer;
	if (!buffer->uncached || buffer->address == NULL ||
	    begin > buffer->bytes || bytes > buffer->bytes - begin)
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
	if (info.sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || info.flags != 0 ||
	    info.size == 0 || info.size > VULKAN_RESOURCE_BYTES || info.usage == 0 ||
	    (info.usage & ~VULKAN_BUFFER_USAGE) != 0)
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

/* Creates one single-level, single-layer raster colour image with exact native row pitch. */
static int
create_image(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct bcm2711_vulkan_resource description;
	uint64_t device_id;
	uint64_t present;
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
	if (info.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || info.flags != 0 ||
	    info.imageType != VK_IMAGE_TYPE_2D || info.extent.width == 0 || info.extent.width > 4096 ||
	    info.extent.height == 0 || info.extent.height > 4096 || info.extent.depth != 1 ||
	    info.mipLevels != 1 || info.arrayLayers != 1 || info.samples != VK_SAMPLE_COUNT_1_BIT)
		return ENOTSUP;
	if (info.format != VK_FORMAT_R8G8B8A8_UNORM && info.format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;
	if (info.tiling != VK_IMAGE_TILING_LINEAR && info.tiling != VK_IMAGE_TILING_OPTIMAL)
		return ENOTSUP;
	if (info.usage == 0 || (info.usage & ~VULKAN_IMAGE_USAGE) != 0)
		return ENOTSUP;
	if (info.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
	    (info.initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED || info.tiling != VK_IMAGE_TILING_LINEAR))
		return ENOTSUP;
	error = sharing_mode(info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices);
	if (error != 0)
		return error;

	/* Optimal images use the same native raster backing; Vulkan does not require exposing their CPU subresource layout. */
	kern_memset(&description, 0, sizeof(description));
	description.width = info.extent.width;
	description.height = info.extent.height;
	description.pitch = (info.extent.width * 4U + 63U) & ~63U;
	description.bytes = (uint64_t)description.pitch * info.extent.height;
	description.required_bytes = description.bytes;
	description.alignment = 64;
	description.usage = info.usage;
	description.format = info.format;
	description.tiling = info.tiling;
	description.layout = info.initialLayout;
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
	if (reader->error != 0 || allocator != 0 || present != 1 || identity == 0)
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
	if (resource == NULL || object == NULL || resource->memory != NULL)
		return EINVAL;
	memory = object->payload;
	if (memory->device != resource->device)
		return EINVAL;

	/* Unsupported, misaligned or partial intervals never mutate the existing unbound resource. */
	if (memory->view == NULL || memory->view->quarantined ||
	    (offset & (resource->alignment - 1U)) != 0 ||
	    offset > memory->bytes || resource->required_bytes > memory->bytes - offset) {
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

	/* Both generated input and output pointers must describe the one supported complete colour subresource. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	input = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || input != 1)
		return EINVAL;
	kern_memset(&subresource, 0, sizeof(subresource));
	i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);
	output = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || output != 1 ||
	    subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
	    subresource.mipLevel != 0 || subresource.arrayLayer != 0)
		return EINVAL;
	resource = find_resource(session, I915_VK_OBJ_IMAGE, device_id, identity);
	if (resource == NULL || resource->tiling != VK_IMAGE_TILING_LINEAR)
		return ENOTSUP;

	/* The first subresource starts at zero relative to the binding, never at a host pointer or GPU VA. */
	kern_memset(&layout, 0, sizeof(layout));
	layout.size = resource->bytes;
	layout.rowPitch = resource->pitch;
	layout.arrayPitch = resource->bytes;
	layout.depthPitch = resource->bytes;
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
	if (checked.error != 0 || type != expected || next != 0)
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
	if (mode != VK_SHARING_MODE_CONCURRENT || count != 1 || families == NULL || families[0] != 0)
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
}
