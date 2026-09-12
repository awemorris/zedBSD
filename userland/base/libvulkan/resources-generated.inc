/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Maintained ordinary resource wrappers; input structures use the common typed codec.
 */

/*
 * Creates one ordinary VkBuffer from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(
	VkDevice device,
	const VkBufferCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkBuffer *pBuffer)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_BUFFER, sizeof(struct vulkan_object), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateBuffer);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyBuffer);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pBuffer = (VkBuffer)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkBuffer with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(
	VkDevice device,
	VkBuffer resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroyBuffer, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}

/*
 * Creates one ordinary VkBufferView from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBufferView(
	VkDevice device,
	const VkBufferViewCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkBufferView *pBufferView)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_BUFFER_VIEW, sizeof(struct vulkan_object), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateBufferView);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkBufferViewCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyBufferView);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pBufferView = (VkBufferView)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkBufferView with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyBufferView(
	VkDevice device,
	VkBufferView resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroyBufferView, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}

/*
 * Creates one ordinary VkImage from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(
	VkDevice device,
	const VkImageCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkImage *pImage)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;
	struct vulkan_image *image;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_IMAGE, sizeof(struct vulkan_image), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateImage);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyImage);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Retains ordinary image metadata required by WSI and later standard resource operations. */
	image = (struct vulkan_image *)object;
	image->format = pCreateInfo->format;
	image->type = pCreateInfo->imageType;
	image->extent = pCreateInfo->extent;
	image->mip_levels = pCreateInfo->mipLevels;
	image->array_layers = pCreateInfo->arrayLayers;
	image->samples = pCreateInfo->samples;
	image->usage = pCreateInfo->usage;
	image->tiling = pCreateInfo->tiling;
	image->sharing_mode = pCreateInfo->sharingMode;

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pImage = (VkImage)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkImage with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(
	VkDevice device,
	VkImage resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroyImage, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}

/*
 * Creates one ordinary VkImageView from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(
	VkDevice device,
	const VkImageViewCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkImageView *pImageView)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_IMAGE_VIEW, sizeof(struct vulkan_object), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateImageView);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkImageViewCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyImageView);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pImageView = (VkImageView)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkImageView with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(
	VkDevice device,
	VkImageView resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroyImageView, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}

/*
 * Creates one ordinary VkShaderModule from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(
	VkDevice device,
	const VkShaderModuleCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkShaderModule *pShaderModule)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_SHADER_MODULE, sizeof(struct vulkan_object), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateShaderModule);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkShaderModuleCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyShaderModule);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pShaderModule = (VkShaderModule)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkShaderModule with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(
	VkDevice device,
	VkShaderModule resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroyShaderModule, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}

/*
 * Creates one ordinary VkSampler from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(
	VkDevice device,
	const VkSamplerCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSampler *pSampler)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_SAMPLER, sizeof(struct vulkan_object), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateSampler);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkSamplerCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroySampler);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pSampler = (VkSampler)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkSampler with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroySampler(
	VkDevice device,
	VkSampler resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroySampler, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}

/*
 * Creates one ordinary VkFramebuffer from all caller-supplied standard parameters.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFramebuffer(
	VkDevice device,
	const VkFramebufferCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFramebuffer *pFramebuffer)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves local ownership before any renderer creation can have side effects. */
	owner = vulkan_device(device);
	status = resource_allocate(owner, VULKAN_OBJECT_FRAMEBUFFER, sizeof(struct vulkan_object), pAllocator, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Encodes every active field using its declared type and protocol array count. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateFramebuffer);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkFramebufferCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyFramebuffer);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes caller output only after local and native creation succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pFramebuffer = (VkFramebuffer)(uintptr_t)handle;

	/* Succeeded: the public handle owns one real native resource. */
	return VK_SUCCESS;
}

/*
 * Destroys one ordinary VkFramebuffer with the command's compatible callback policy.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyFramebuffer(
	VkDevice device,
	VkFramebuffer resource,
	const VkAllocationCallbacks *pAllocator)
{
	/* Consumes native and local ownership without imposing GPU-specific publication rules. */
	resource_destroy(vulkan_device(device), (uint64_t)resource, VULKAN_OPCODE_vkDestroyFramebuffer, pAllocator);

	/* Succeeded: a null handle is ignored and ordinary ownership is released. */
	return;
}
