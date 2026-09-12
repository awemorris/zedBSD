/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements ordinary Vulkan resource ownership and native memory requirements.
 */

#include <string.h>
#include "internal.h"

/* Retains only metadata needed to interpret active graphics and clear parameters. */
struct vulkan_render_pass {
	struct vulkan_object object;
	uint32_t attachment_count;
	VkAttachmentDescription *attachments;
	uint32_t subpass_count;
	VkBool32 *color;
	VkBool32 *depth_stencil;
};

static VkResult resource_allocate(struct VkDevice_T *device, enum vulkan_object_kind kind, size_t bytes, const VkAllocationCallbacks *allocator, struct vulkan_object **result);
static void resource_destroy(struct VkDevice_T *device, uint64_t handle, uint32_t opcode, const VkAllocationCallbacks *allocator);
static VkResult resource_bind(struct VkDevice_T *device, uint64_t resource, VkDeviceMemory memory, VkDeviceSize offset, uint32_t opcode);
static void resource_requirements(struct VkDevice_T *device, uint64_t resource, uint32_t opcode, VkMemoryRequirements *requirements);
static VkResult render_pass_metadata(struct vulkan_render_pass *pass, const VkRenderPassCreateInfo *info);
static void render_pass_free(struct vulkan_render_pass *pass);

#include "resources-generated.inc"

/*
 * Creates a render pass while retaining the attachment semantics used by later commands.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass(
	VkDevice device,
	const VkRenderPassCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkRenderPass *pRenderPass)
{
	struct VkDevice_T *owner;
	struct vulkan_render_pass *pass;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Allocates all local metadata before a native render-pass object can be created. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_RENDER_PASS, sizeof(*pass), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Copies the exact caller attachment definitions without hardcoded scene formats. */
	pass = (struct vulkan_render_pass *)object;
	status = render_pass_metadata(pass, pCreateInfo);
	if (status != VK_SUCCESS) {
		render_pass_free(pass);
		return status;
	}

	/* Encodes attachment layouts through the common direct-display preservation mapping. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateRenderPass);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkRenderPassCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyRenderPass);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		render_pass_free(pass);
		return status;
	}

	/* Publishes only a complete native render pass with matching immutable local metadata. */
	handle = vulkan_nondispatchable_handle(object);
	*pRenderPass = (VkRenderPass)(uintptr_t)handle;

	/* Succeeded: arbitrary graphics pipelines and clear commands can consult this render pass. */
	return VK_SUCCESS;
}

/*
 * Destroys a render pass and every local attachment/subpass allocation it owns.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyRenderPass(
	VkDevice device,
	VkRenderPass renderPass,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_render_pass *pass;
	VkResult status;

	/* A null render pass has no native object or metadata lifetime. */
	owner = vulkan_device(device);
	pass = (struct vulkan_render_pass *)vulkan_nondispatchable_object((uint64_t)renderPass);
	if (pass == NULL)
		return;

	/* Applies the current compatible callback policy to all private metadata frees. */
	if (pAllocator != NULL) {
		pass->object.allocator.callbacks = *pAllocator;
		pass->object.allocator.has_callbacks = VK_TRUE;
	}

	/* Consumes the native object before returning its local metadata to the caller's allocator. */
	status = vulkan_object_destroy_remote(owner, &pass->object, VULKAN_OPCODE_vkDestroyRenderPass);
	(void)status;
	render_pass_free(pass);

	/* Succeeded: this render pass retains no native or local ownership. */
	return;
}

/*
 * Binds one ordinary buffer to the caller-selected memory allocation and offset.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(
	VkDevice device,
	VkBuffer buffer,
	VkDeviceMemory memory,
	VkDeviceSize memoryOffset)
{
	struct VkDevice_T *owner;
	VkResult status;

	/* Native binding validates the same type index and resource requirements exposed to the caller. */
	owner = vulkan_device(device);
	status = resource_bind(owner, (uint64_t)buffer, memory, memoryOffset, VULKAN_OPCODE_vkBindBufferMemory);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the resource is bound to the caller-selected native allocation. */
	return VK_SUCCESS;
}

/*
 * Binds one ordinary image without imposing a WSI-specific allocation path.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(
	VkDevice device,
	VkImage image,
	VkDeviceMemory memory,
	VkDeviceSize memoryOffset)
{
	struct VkDevice_T *owner;
	VkResult status;

	/* Presentable and offscreen images use exactly the same standard memory binding operation. */
	owner = vulkan_device(device);
	status = resource_bind(owner, (uint64_t)image, memory, memoryOffset, VULKAN_OPCODE_vkBindImageMemory);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the resource is bound to the caller-selected native allocation. */
	return VK_SUCCESS;
}

/*
 * Returns native buffer size, alignment, and unmodified memory-type index bits.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(
	VkDevice device,
	VkBuffer buffer,
	VkMemoryRequirements *pMemoryRequirements)
{
	/* Queries the actual created resource rather than predicting requirements from its size. */
	resource_requirements(vulkan_device(device), (uint64_t)buffer, VULKAN_OPCODE_vkGetBufferMemoryRequirements, pMemoryRequirements);

	/* Succeeded: the output is fully initialized by the common typed query path. */
	return;
}

/*
 * Returns native image requirements for the complete caller-selected image configuration.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(
	VkDevice device,
	VkImage image,
	VkMemoryRequirements *pMemoryRequirements)
{
	/* Native tiling, mip levels, layers, samples, and formats determine these requirements. */
	resource_requirements(vulkan_device(device), (uint64_t)image, VULKAN_OPCODE_vkGetImageMemoryRequirements, pMemoryRequirements);

	/* Succeeded: the output retains the renderer's original memory type indices. */
	return;
}

/*
 * Retrieves a linear image subresource's native row, array, and depth pitches.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetImageSubresourceLayout(
	VkDevice device,
	VkImage image,
	const VkImageSubresource *pSubresource,
	VkSubresourceLayout *pLayout)
{
	struct VkDevice_T *owner;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkSubresourceLayout layout;
	VkResult status;
	VkBool32 present;

	/* Requests the selected aspect, mip level, and array layer from the real native image. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetImageSubresourceLayout);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_encode_handle(&writer, (uint64_t)image);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageSubresource(&writer, pSubresource);
	vulkan_write_u64(&writer, 1);
	status = vulkan_command_execute(owner->object.context, &writer, 52, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* Uses private output storage until every native pitch and offset was decoded. */
	memset(&layout, 0, sizeof(layout));
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkSubresourceLayout(&reader, &layout);
	}

	/* A failed response cannot expose partially initialized layout values. */
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		memset(&layout, 0, sizeof(layout));

	/* Publishes the complete native subresource layout in the public ABI. */
	*pLayout = layout;

	/* Succeeded: no application-visible pitch was inferred from a sample scene. */
	return;
}

/*
 * Returns native render-area granularity for a specific created render pass.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetRenderAreaGranularity(
	VkDevice device,
	VkRenderPass renderPass,
	VkExtent2D *pGranularity)
{
	struct VkDevice_T *owner;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkExtent2D extent;
	VkResult status;
	VkBool32 present;

	/* Retrieves the actual render pass's native implementation granularity. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetRenderAreaGranularity);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_encode_handle(&writer, (uint64_t)renderPass);
	vulkan_write_u64(&writer, 1);
	status = vulkan_command_execute(owner->object.context, &writer, 20, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* Decodes both extent components before publishing either one. */
	memset(&extent, 0, sizeof(extent));
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkExtent2D(&reader, &extent);
	}

	/* Avoids leaking partial response state through this void output API. */
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		memset(&extent, 0, sizeof(extent));

	/* Publishes exactly the queried native granularity. */
	*pGranularity = extent;

	/* Succeeded: the output was obtained from this render pass's implementation. */
	return;
}

/*
 * Enumerates sparse image memory requirements with bounded native output storage.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetImageSparseMemoryRequirements(
	VkDevice device,
	VkImage image,
	uint32_t *pSparseMemoryRequirementCount,
	VkSparseImageMemoryRequirements *pSparseMemoryRequirements)
{
	struct VkDevice_T *owner;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint32_t capacity;
	uint32_t count;
	uint32_t index;
	uint64_t array_count;
	size_t reply_bytes;
	VkBool32 present;
	VkResult status;

	/* A count-only call never reads an uninitialized input count. */
	capacity = 0;
	if (pSparseMemoryRequirements != NULL) {
		capacity = *pSparseMemoryRequirementCount;

		/* Wire array zero aliases a null count query, so an empty caller array is handled locally. */
		if (capacity == 0) {
			*pSparseMemoryRequirementCount = 0;
			return;
		}
	}

	/* Checks fixed element wire size before allocating a dynamically sized response. */
	reply_bytes = (size_t)capacity * 48 + 24;
	if (reply_bytes < 24 || (reply_bytes - 24) / 48 != capacity) {
		*pSparseMemoryRequirementCount = 0;
		return;
	}

	/* Requests the actual resource's sparse requirements, including the zero-count case. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetImageSparseMemoryRequirements);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_encode_handle(&writer, (uint64_t)image);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, capacity);
	vulkan_write_u64(&writer, capacity);
	status = vulkan_command_execute(owner->object.context, &writer, reply_bytes, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	count = 0;
	if (status == VK_SUCCESS) {
		/* Validates count and array markers before writing any caller element. */
		present = vulkan_reply_pointer(&reader);
		count = vulkan_read_u32(&reader);
		array_count = vulkan_read_u64(&reader);
		if (!present ||
		    (pSparseMemoryRequirements == NULL && array_count != 0) ||
		    (pSparseMemoryRequirements != NULL &&
		     (count > capacity || array_count != count)))
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Each sparse requirement has a fixed fully typed forty-eight-byte wire record. */
		if (pSparseMemoryRequirements != NULL) {
			for (index = 0;
			     index < count && reader.error == VK_SUCCESS;
			     index++) {
				vulkan_decode_VkSparseImageMemoryRequirements(&reader, &pSparseMemoryRequirements[index]);
			}
		}
	}

	/* A malformed or unavailable response yields no valid returned elements. */
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		count = 0;

	/* Publishes the actual count for both count-only and bounded-array calls. */
	*pSparseMemoryRequirementCount = count;

	/* Succeeded: every reported sparse requirement belongs to the requested image. */
	return;
}

/*
 * Enumerates sparse image format properties for arbitrary standard input combinations.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceSparseImageFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkImageType type,
	VkSampleCountFlagBits samples,
	VkImageUsageFlags usage,
	VkImageTiling tiling,
	uint32_t *pPropertyCount,
	VkSparseImageFormatProperties *pProperties)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint32_t capacity;
	uint32_t count;
	uint32_t index;
	uint64_t array_count;
	size_t reply_bytes;
	VkBool32 present;
	VkResult status;

	/* Count-only enumeration does not consume the caller's output count as input. */
	capacity = 0;
	if (pProperties != NULL) {
		capacity = *pPropertyCount;

		/* Preserves a real zero-capacity array instead of issuing an indistinguishable null query. */
		if (capacity == 0) {
			*pPropertyCount = 0;
			return;
		}
	}

	/* Computes a checked response size from the protocol's twenty-byte property record. */
	reply_bytes = (size_t)capacity * 20 + 24;
	if (reply_bytes < 24 || (reply_bytes - 24) / 20 != capacity) {
		*pPropertyCount = 0;
		return;
	}

	/* Sends every format, usage, and sample-count input without guessing support. */
	physical = vulkan_physical_device(physicalDevice);
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetPhysicalDeviceSparseImageFormatProperties);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u32(&writer, format);
	vulkan_write_u32(&writer, type);
	vulkan_write_u32(&writer, samples);
	vulkan_write_u32(&writer, usage);
	vulkan_write_u32(&writer, tiling);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, capacity);
	vulkan_write_u64(&writer, capacity);
	status = vulkan_command_execute(physical->object.context, &writer, reply_bytes, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	count = 0;
	if (status == VK_SUCCESS) {
		/* Validates the returned extent before any array element is copied. */
		present = vulkan_reply_pointer(&reader);
		count = vulkan_read_u32(&reader);
		array_count = vulkan_read_u64(&reader);
		if (!present ||
		    (pProperties == NULL && array_count != 0) ||
		    (pProperties != NULL &&
		     (count > capacity || array_count != count)))
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Decodes exactly the complete elements accepted by the caller's capacity. */
		if (pProperties != NULL) {
			for (index = 0;
			     index < count && reader.error == VK_SUCCESS;
			     index++) {
				vulkan_decode_VkSparseImageFormatProperties(&reader, &pProperties[index]);
			}
		}
	}

	/* Does not report partially decoded entries as a successful sparse capability set. */
	status = vulkan_reply_finish(physical->object.context, &reader, status);
	if (status != VK_SUCCESS)
		count = 0;

	/* Returns the full count for a null array or the actual bounded number written. */
	*pPropertyCount = count;

	/* Succeeded: unsupported format combinations naturally return an empty property set. */
	return;
}

/*
 * Reports which optional graphics state is actually used by one render-pass subpass.
 */
VkResult
vulkan_render_pass_subpass(
	VkRenderPass render_pass,
	uint32_t index,
	VkBool32 *color,
	VkBool32 *depth_stencil)
{
	struct vulkan_render_pass *pass;

	/* Resolves ordinary object metadata without inspecting any native host handle. */
	pass = (struct vulkan_render_pass *)vulkan_nondispatchable_object((uint64_t)render_pass);
	if (pass == NULL ||
	    pass->object.kind != VULKAN_OBJECT_RENDER_PASS ||
	    index >= pass->subpass_count)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Graphics pipeline encoding uses these flags to avoid dereferencing ignored state pointers. */
	*color = pass->color[index];
	*depth_stencil = pass->depth_stencil[index];

	/* Succeeded: both flags describe the selected created render-pass subpass. */
	return VK_SUCCESS;
}

/*
 * Returns immutable attachment semantics for interpreting a command's active clear union.
 */
const VkAttachmentDescription *
vulkan_render_pass_attachment(
	VkRenderPass render_pass,
	uint32_t index)
{
	struct vulkan_render_pass *pass;

	/* Extra clear-value entries beyond the attachment count are ignored by the standard API. */
	pass = (struct vulkan_render_pass *)vulkan_nondispatchable_object((uint64_t)render_pass);
	if (pass == NULL || index >= pass->attachment_count)
		return NULL;

	/* Succeeded: the returned metadata remains owned by the render-pass lifetime. */
	return &pass->attachments[index];
}

/* Preallocates the common ownership record and a never-reused native identity. */
static VkResult
resource_allocate(
	struct VkDevice_T *device,
	enum vulkan_object_kind kind,
	size_t bytes,
	const VkAllocationCallbacks *allocator,
	struct vulkan_object **result)
{
	struct vulkan_object *object;
	VkResult status;

	/* All ordinary resources inherit the actual logical-device allocator when needed. */
	status = vulkan_object_alloc(bytes, __alignof__(struct vulkan_render_pass), kind, &device->object, device->object.context, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* A failed identity reservation cannot leave an inaccessible local allocation. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes output only after the complete local pre-creation record exists. */
	*result = object;

	/* Succeeded: typed input encoding may now safely request native creation. */
	return VK_SUCCESS;
}

/* Destroys native resource ownership before returning local object storage to its allocator. */
static void
resource_destroy(
	struct VkDevice_T *device,
	uint64_t handle,
	uint32_t opcode,
	const VkAllocationCallbacks *allocator)
{
	struct vulkan_object *object;
	VkResult status;

	/* Destruction accepts the standard null handle without a protocol operation. */
	object = vulkan_nondispatchable_object(handle);
	if (object == NULL)
		return;

	/* Both temporary native-command allocations and the final free use this compatible policy. */
	if (allocator != NULL) {
		object->allocator.callbacks = *allocator;
		object->allocator.has_callbacks = VK_TRUE;
	}

	/* A failed renderer namespace remains owned by its session until instance cleanup. */
	status = vulkan_object_destroy_remote(device, object, opcode);
	(void)status;
	vulkan_object_free(object);

	/* Succeeded: no live public resource refers to the released local allocation. */
	return;
}

/* Performs the native bind operation for an ordinary buffer or image identity. */
static VkResult
resource_bind(
	struct VkDevice_T *device,
	uint64_t resource,
	VkDeviceMemory memory,
	VkDeviceSize offset,
	uint32_t opcode)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Retains original memory type indices by using the actual native memory object. */
	vulkan_writer_init_for_object(&writer, &device->object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_encode_handle(&writer, resource);
	vulkan_encode_handle(&writer, (uint64_t)memory);
	vulkan_write_u64(&writer, offset);
	status = vulkan_command_execute(device->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(device->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the native resource is bound at the caller's exact memory offset. */
	return VK_SUCCESS;
}

/* Queries actual native memory requirements into a complete public ABI structure. */
static void
resource_requirements(
	struct VkDevice_T *device,
	uint64_t resource,
	uint32_t opcode,
	VkMemoryRequirements *requirements)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkMemoryRequirements result;
	VkResult status;
	VkBool32 present;

	/* Requests one native requirement record for the existing resource object. */
	vulkan_writer_init_for_object(&writer, &device->object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_encode_handle(&writer, resource);
	vulkan_write_u64(&writer, 1);
	status = vulkan_command_execute(device->object.context, &writer, 32, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);

	/* Decodes exact field widths before changing the application's output structure. */
	memset(&result, 0, sizeof(result));
	if (status == VK_SUCCESS) {
		present = vulkan_reply_pointer(&reader);
		if (present)
			vulkan_decode_VkMemoryRequirements(&reader, &result);
	}

	/* Zeroes all fields if the response cannot describe a valid complete requirement set. */
	status = vulkan_reply_finish(device->object.context, &reader, status);
	if (status != VK_SUCCESS)
		memset(&result, 0, sizeof(result));

	/* Publishes the native size, alignment, and memoryTypeBits together. */
	*requirements = result;

	/* Succeeded: no type bit was silently renumbered by the guest implementation. */
	return;
}

/* Copies attachment and subpass metadata before native render-pass creation can succeed. */
static VkResult
render_pass_metadata(
	struct vulkan_render_pass *pass,
	const VkRenderPassCreateInfo *info)
{
	const VkSubpassDescription *subpass;
	uint32_t index;
	uint32_t color;
	size_t bytes;

	/* Empty attachment sets need no host allocation and remain valid render passes. */
	pass->attachment_count = info->attachmentCount;
	if (info->attachmentCount != 0) {
		/* Checks the exact public-record copy extent before invoking callbacks. */
		bytes = (size_t)info->attachmentCount * sizeof(*pass->attachments);
		if (bytes / sizeof(*pass->attachments) != info->attachmentCount)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Retains active clear/load semantics for the lifetime of this render pass. */
		pass->attachments = vulkan_allocate(&pass->object.allocator, bytes, __alignof__(VkAttachmentDescription), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if (pass->attachments == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		memcpy(pass->attachments, info->pAttachments, bytes);
	}

	/* Every render pass has at least one subpass under the standard creation contract. */
	pass->subpass_count = info->subpassCount;
	bytes = (size_t)info->subpassCount * sizeof(*pass->color);
	if (bytes / sizeof(*pass->color) != info->subpassCount)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Keeps independently owned color and depth-state activation records. */
	pass->color = vulkan_allocate(&pass->object.allocator, bytes, __alignof__(VkBool32), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (pass->color == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	pass->depth_stencil = vulkan_allocate(&pass->object.allocator, bytes, __alignof__(VkBool32), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (pass->depth_stencil == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Derives active graphics state from used attachment references, never from pointer garbage. */
	memset(pass->color, 0, bytes);
	memset(pass->depth_stencil, 0, bytes);
	for (index = 0; index < info->subpassCount; index++) {
		subpass = &info->pSubpasses[index];

		/* UNUSED color references do not require dereferencing an ignored color-blend state. */
		for (color = 0; color < subpass->colorAttachmentCount; color++) {
			if (subpass->pColorAttachments[color].attachment != VK_ATTACHMENT_UNUSED)
				pass->color[index] = VK_TRUE;
		}

		/* A depth/stencil state is active only when a real depth/stencil attachment is used. */
		if (subpass->pDepthStencilAttachment != NULL) {
			if (subpass->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
				pass->depth_stencil[index] = VK_TRUE;
		}
	}

	/* Succeeded: every subpass can determine which caller state pointers are meaningful. */
	return VK_SUCCESS;
}

/* Releases all private render-pass metadata through its current effective callback policy. */
static void
render_pass_free(
	struct vulkan_render_pass *pass)
{
	/* Arrays have independent nullable ownership during partial metadata allocation. */
	vulkan_free(&pass->object.allocator, pass->depth_stencil);
	vulkan_free(&pass->object.allocator, pass->color);
	vulkan_free(&pass->object.allocator, pass->attachments);
	vulkan_object_free(&pass->object);

	/* Succeeded: no render-pass metadata remains allocated after local destruction. */
	return;
}
