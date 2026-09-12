/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Maintained by tools/maintain-codec.noct from declarations and explicit protocol rules.
 */

#ifndef VULKAN_CODEC_H
#define VULKAN_CODEC_H

void
vulkan_encode_handle(
	struct vulkan_writer *writer,
	uint64_t handle);

void
vulkan_encode_VkExtent2D(
	struct vulkan_writer *writer,
	const VkExtent2D *record);

void
vulkan_decode_VkExtent2D(
	struct vulkan_reader *reader,
	VkExtent2D *record);

void
vulkan_encode_VkExtent3D(
	struct vulkan_writer *writer,
	const VkExtent3D *record);

void
vulkan_decode_VkExtent3D(
	struct vulkan_reader *reader,
	VkExtent3D *record);

void
vulkan_encode_VkOffset2D(
	struct vulkan_writer *writer,
	const VkOffset2D *record);

void
vulkan_decode_VkOffset2D(
	struct vulkan_reader *reader,
	VkOffset2D *record);

void
vulkan_encode_VkOffset3D(
	struct vulkan_writer *writer,
	const VkOffset3D *record);

void
vulkan_decode_VkOffset3D(
	struct vulkan_reader *reader,
	VkOffset3D *record);

void
vulkan_encode_VkRect2D(
	struct vulkan_writer *writer,
	const VkRect2D *record);

void
vulkan_decode_VkRect2D(
	struct vulkan_reader *reader,
	VkRect2D *record);

void
vulkan_encode_VkBufferMemoryBarrier(
	struct vulkan_writer *writer,
	const VkBufferMemoryBarrier *record);

void
vulkan_encode_VkDispatchIndirectCommand(
	struct vulkan_writer *writer,
	const VkDispatchIndirectCommand *record);

void
vulkan_decode_VkDispatchIndirectCommand(
	struct vulkan_reader *reader,
	VkDispatchIndirectCommand *record);

void
vulkan_encode_VkDrawIndexedIndirectCommand(
	struct vulkan_writer *writer,
	const VkDrawIndexedIndirectCommand *record);

void
vulkan_decode_VkDrawIndexedIndirectCommand(
	struct vulkan_reader *reader,
	VkDrawIndexedIndirectCommand *record);

void
vulkan_encode_VkDrawIndirectCommand(
	struct vulkan_writer *writer,
	const VkDrawIndirectCommand *record);

void
vulkan_decode_VkDrawIndirectCommand(
	struct vulkan_reader *reader,
	VkDrawIndirectCommand *record);

void
vulkan_encode_VkImageSubresourceRange(
	struct vulkan_writer *writer,
	const VkImageSubresourceRange *record);

void
vulkan_decode_VkImageSubresourceRange(
	struct vulkan_reader *reader,
	VkImageSubresourceRange *record);

void
vulkan_encode_VkImageMemoryBarrier(
	struct vulkan_writer *writer,
	const VkImageMemoryBarrier *record);

void
vulkan_encode_VkMemoryBarrier(
	struct vulkan_writer *writer,
	const VkMemoryBarrier *record);

void
vulkan_encode_VkPipelineCacheHeaderVersionOne(
	struct vulkan_writer *writer,
	const VkPipelineCacheHeaderVersionOne *record);

void
vulkan_decode_VkPipelineCacheHeaderVersionOne(
	struct vulkan_reader *reader,
	VkPipelineCacheHeaderVersionOne *record);

void
vulkan_encode_VkApplicationInfo(
	struct vulkan_writer *writer,
	const VkApplicationInfo *record);

void
vulkan_encode_VkFormatProperties(
	struct vulkan_writer *writer,
	const VkFormatProperties *record);

void
vulkan_decode_VkFormatProperties(
	struct vulkan_reader *reader,
	VkFormatProperties *record);

void
vulkan_encode_VkImageFormatProperties(
	struct vulkan_writer *writer,
	const VkImageFormatProperties *record);

void
vulkan_decode_VkImageFormatProperties(
	struct vulkan_reader *reader,
	VkImageFormatProperties *record);

void
vulkan_encode_VkInstanceCreateInfo(
	struct vulkan_writer *writer,
	const VkInstanceCreateInfo *record);

void
vulkan_encode_VkMemoryHeap(
	struct vulkan_writer *writer,
	const VkMemoryHeap *record);

void
vulkan_decode_VkMemoryHeap(
	struct vulkan_reader *reader,
	VkMemoryHeap *record);

void
vulkan_encode_VkMemoryType(
	struct vulkan_writer *writer,
	const VkMemoryType *record);

void
vulkan_decode_VkMemoryType(
	struct vulkan_reader *reader,
	VkMemoryType *record);

void
vulkan_encode_VkPhysicalDeviceFeatures(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceFeatures *record);

void
vulkan_decode_VkPhysicalDeviceFeatures(
	struct vulkan_reader *reader,
	VkPhysicalDeviceFeatures *record);

void
vulkan_encode_VkPhysicalDeviceLimits(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceLimits *record);

void
vulkan_decode_VkPhysicalDeviceLimits(
	struct vulkan_reader *reader,
	VkPhysicalDeviceLimits *record);

void
vulkan_encode_VkPhysicalDeviceMemoryProperties(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceMemoryProperties *record);

void
vulkan_decode_VkPhysicalDeviceMemoryProperties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceMemoryProperties *record);

void
vulkan_encode_VkPhysicalDeviceSparseProperties(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceSparseProperties *record);

void
vulkan_decode_VkPhysicalDeviceSparseProperties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceSparseProperties *record);

void
vulkan_encode_VkPhysicalDeviceProperties(
	struct vulkan_writer *writer,
	const VkPhysicalDeviceProperties *record);

void
vulkan_decode_VkPhysicalDeviceProperties(
	struct vulkan_reader *reader,
	VkPhysicalDeviceProperties *record);

void
vulkan_encode_VkQueueFamilyProperties(
	struct vulkan_writer *writer,
	const VkQueueFamilyProperties *record);

void
vulkan_decode_VkQueueFamilyProperties(
	struct vulkan_reader *reader,
	VkQueueFamilyProperties *record);

void
vulkan_encode_VkDeviceQueueCreateInfo(
	struct vulkan_writer *writer,
	const VkDeviceQueueCreateInfo *record);

void
vulkan_encode_VkDeviceCreateInfo(
	struct vulkan_writer *writer,
	const VkDeviceCreateInfo *record);

void
vulkan_encode_VkExtensionProperties(
	struct vulkan_writer *writer,
	const VkExtensionProperties *record);

void
vulkan_decode_VkExtensionProperties(
	struct vulkan_reader *reader,
	VkExtensionProperties *record);

void
vulkan_encode_VkLayerProperties(
	struct vulkan_writer *writer,
	const VkLayerProperties *record);

void
vulkan_decode_VkLayerProperties(
	struct vulkan_reader *reader,
	VkLayerProperties *record);

void
vulkan_encode_VkSubmitInfo(
	struct vulkan_writer *writer,
	const VkSubmitInfo *record);

void
vulkan_encode_VkMappedMemoryRange(
	struct vulkan_writer *writer,
	const VkMappedMemoryRange *record);

void
vulkan_encode_VkMemoryAllocateInfo(
	struct vulkan_writer *writer,
	const VkMemoryAllocateInfo *record);

void
vulkan_encode_VkMemoryRequirements(
	struct vulkan_writer *writer,
	const VkMemoryRequirements *record);

void
vulkan_decode_VkMemoryRequirements(
	struct vulkan_reader *reader,
	VkMemoryRequirements *record);

void
vulkan_encode_VkSparseMemoryBind(
	struct vulkan_writer *writer,
	const VkSparseMemoryBind *record);

void
vulkan_encode_VkSparseBufferMemoryBindInfo(
	struct vulkan_writer *writer,
	const VkSparseBufferMemoryBindInfo *record);

void
vulkan_encode_VkSparseImageOpaqueMemoryBindInfo(
	struct vulkan_writer *writer,
	const VkSparseImageOpaqueMemoryBindInfo *record);

void
vulkan_encode_VkImageSubresource(
	struct vulkan_writer *writer,
	const VkImageSubresource *record);

void
vulkan_decode_VkImageSubresource(
	struct vulkan_reader *reader,
	VkImageSubresource *record);

void
vulkan_encode_VkSparseImageMemoryBind(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryBind *record);

void
vulkan_encode_VkSparseImageMemoryBindInfo(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryBindInfo *record);

void
vulkan_encode_VkBindSparseInfo(
	struct vulkan_writer *writer,
	const VkBindSparseInfo *record);

void
vulkan_encode_VkSparseImageFormatProperties(
	struct vulkan_writer *writer,
	const VkSparseImageFormatProperties *record);

void
vulkan_decode_VkSparseImageFormatProperties(
	struct vulkan_reader *reader,
	VkSparseImageFormatProperties *record);

void
vulkan_encode_VkSparseImageMemoryRequirements(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryRequirements *record);

void
vulkan_decode_VkSparseImageMemoryRequirements(
	struct vulkan_reader *reader,
	VkSparseImageMemoryRequirements *record);

void
vulkan_encode_VkFenceCreateInfo(
	struct vulkan_writer *writer,
	const VkFenceCreateInfo *record);

void
vulkan_encode_VkSemaphoreCreateInfo(
	struct vulkan_writer *writer,
	const VkSemaphoreCreateInfo *record);

void
vulkan_encode_VkEventCreateInfo(
	struct vulkan_writer *writer,
	const VkEventCreateInfo *record);

void
vulkan_encode_VkQueryPoolCreateInfo(
	struct vulkan_writer *writer,
	const VkQueryPoolCreateInfo *record);

void
vulkan_encode_VkBufferCreateInfo(
	struct vulkan_writer *writer,
	const VkBufferCreateInfo *record);

void
vulkan_encode_VkBufferViewCreateInfo(
	struct vulkan_writer *writer,
	const VkBufferViewCreateInfo *record);

void
vulkan_encode_VkImageCreateInfo(
	struct vulkan_writer *writer,
	const VkImageCreateInfo *record);

void
vulkan_encode_VkSubresourceLayout(
	struct vulkan_writer *writer,
	const VkSubresourceLayout *record);

void
vulkan_decode_VkSubresourceLayout(
	struct vulkan_reader *reader,
	VkSubresourceLayout *record);

void
vulkan_encode_VkComponentMapping(
	struct vulkan_writer *writer,
	const VkComponentMapping *record);

void
vulkan_decode_VkComponentMapping(
	struct vulkan_reader *reader,
	VkComponentMapping *record);

void
vulkan_encode_VkImageViewCreateInfo(
	struct vulkan_writer *writer,
	const VkImageViewCreateInfo *record);

void
vulkan_encode_VkShaderModuleCreateInfo(
	struct vulkan_writer *writer,
	const VkShaderModuleCreateInfo *record);

void
vulkan_encode_VkPipelineCacheCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineCacheCreateInfo *record);

void
vulkan_encode_VkSpecializationMapEntry(
	struct vulkan_writer *writer,
	const VkSpecializationMapEntry *record);

void
vulkan_decode_VkSpecializationMapEntry(
	struct vulkan_reader *reader,
	VkSpecializationMapEntry *record);

void
vulkan_encode_VkSpecializationInfo(
	struct vulkan_writer *writer,
	const VkSpecializationInfo *record);

void
vulkan_encode_VkPipelineShaderStageCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineShaderStageCreateInfo *record);

void
vulkan_encode_VkComputePipelineCreateInfo(
	struct vulkan_writer *writer,
	const VkComputePipelineCreateInfo *record);

void
vulkan_encode_VkVertexInputBindingDescription(
	struct vulkan_writer *writer,
	const VkVertexInputBindingDescription *record);

void
vulkan_decode_VkVertexInputBindingDescription(
	struct vulkan_reader *reader,
	VkVertexInputBindingDescription *record);

void
vulkan_encode_VkVertexInputAttributeDescription(
	struct vulkan_writer *writer,
	const VkVertexInputAttributeDescription *record);

void
vulkan_decode_VkVertexInputAttributeDescription(
	struct vulkan_reader *reader,
	VkVertexInputAttributeDescription *record);

void
vulkan_encode_VkPipelineVertexInputStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineVertexInputStateCreateInfo *record);

void
vulkan_encode_VkPipelineInputAssemblyStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineInputAssemblyStateCreateInfo *record);

void
vulkan_encode_VkPipelineTessellationStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineTessellationStateCreateInfo *record);

void
vulkan_encode_VkViewport(
	struct vulkan_writer *writer,
	const VkViewport *record);

void
vulkan_decode_VkViewport(
	struct vulkan_reader *reader,
	VkViewport *record);

void
vulkan_encode_VkPipelineViewportStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineViewportStateCreateInfo *record);

void
vulkan_encode_VkPipelineRasterizationStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineRasterizationStateCreateInfo *record);

void
vulkan_encode_VkPipelineMultisampleStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineMultisampleStateCreateInfo *record);

void
vulkan_encode_VkStencilOpState(
	struct vulkan_writer *writer,
	const VkStencilOpState *record);

void
vulkan_decode_VkStencilOpState(
	struct vulkan_reader *reader,
	VkStencilOpState *record);

void
vulkan_encode_VkPipelineDepthStencilStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineDepthStencilStateCreateInfo *record);

void
vulkan_encode_VkPipelineColorBlendAttachmentState(
	struct vulkan_writer *writer,
	const VkPipelineColorBlendAttachmentState *record);

void
vulkan_decode_VkPipelineColorBlendAttachmentState(
	struct vulkan_reader *reader,
	VkPipelineColorBlendAttachmentState *record);

void
vulkan_encode_VkPipelineColorBlendStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineColorBlendStateCreateInfo *record);

void
vulkan_encode_VkPipelineDynamicStateCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineDynamicStateCreateInfo *record);

void
vulkan_encode_VkPushConstantRange(
	struct vulkan_writer *writer,
	const VkPushConstantRange *record);

void
vulkan_decode_VkPushConstantRange(
	struct vulkan_reader *reader,
	VkPushConstantRange *record);

void
vulkan_encode_VkPipelineLayoutCreateInfo(
	struct vulkan_writer *writer,
	const VkPipelineLayoutCreateInfo *record);

void
vulkan_encode_VkSamplerCreateInfo(
	struct vulkan_writer *writer,
	const VkSamplerCreateInfo *record);

void
vulkan_encode_VkCopyDescriptorSet(
	struct vulkan_writer *writer,
	const VkCopyDescriptorSet *record);

void
vulkan_encode_VkDescriptorBufferInfo(
	struct vulkan_writer *writer,
	const VkDescriptorBufferInfo *record);

void
vulkan_encode_VkDescriptorPoolSize(
	struct vulkan_writer *writer,
	const VkDescriptorPoolSize *record);

void
vulkan_decode_VkDescriptorPoolSize(
	struct vulkan_reader *reader,
	VkDescriptorPoolSize *record);

void
vulkan_encode_VkDescriptorPoolCreateInfo(
	struct vulkan_writer *writer,
	const VkDescriptorPoolCreateInfo *record);

void
vulkan_encode_VkDescriptorSetAllocateInfo(
	struct vulkan_writer *writer,
	const VkDescriptorSetAllocateInfo *record);

void
vulkan_encode_VkDescriptorSetLayoutBinding(
	struct vulkan_writer *writer,
	const VkDescriptorSetLayoutBinding *record);

void
vulkan_encode_VkDescriptorSetLayoutCreateInfo(
	struct vulkan_writer *writer,
	const VkDescriptorSetLayoutCreateInfo *record);

void
vulkan_encode_VkAttachmentDescription(
	struct vulkan_writer *writer,
	const VkAttachmentDescription *record);

void
vulkan_decode_VkAttachmentDescription(
	struct vulkan_reader *reader,
	VkAttachmentDescription *record);

void
vulkan_encode_VkAttachmentReference(
	struct vulkan_writer *writer,
	const VkAttachmentReference *record);

void
vulkan_decode_VkAttachmentReference(
	struct vulkan_reader *reader,
	VkAttachmentReference *record);

void
vulkan_encode_VkFramebufferCreateInfo(
	struct vulkan_writer *writer,
	const VkFramebufferCreateInfo *record);

void
vulkan_encode_VkSubpassDescription(
	struct vulkan_writer *writer,
	const VkSubpassDescription *record);

void
vulkan_encode_VkSubpassDependency(
	struct vulkan_writer *writer,
	const VkSubpassDependency *record);

void
vulkan_decode_VkSubpassDependency(
	struct vulkan_reader *reader,
	VkSubpassDependency *record);

void
vulkan_encode_VkRenderPassCreateInfo(
	struct vulkan_writer *writer,
	const VkRenderPassCreateInfo *record);

void
vulkan_encode_VkCommandPoolCreateInfo(
	struct vulkan_writer *writer,
	const VkCommandPoolCreateInfo *record);

void
vulkan_encode_VkCommandBufferAllocateInfo(
	struct vulkan_writer *writer,
	const VkCommandBufferAllocateInfo *record);

void
vulkan_encode_VkCommandBufferInheritanceInfo(
	struct vulkan_writer *writer,
	const VkCommandBufferInheritanceInfo *record);

void
vulkan_encode_VkCommandBufferBeginInfo(
	struct vulkan_writer *writer,
	const VkCommandBufferBeginInfo *record);

void
vulkan_encode_VkBufferCopy(
	struct vulkan_writer *writer,
	const VkBufferCopy *record);

void
vulkan_decode_VkBufferCopy(
	struct vulkan_reader *reader,
	VkBufferCopy *record);

void
vulkan_encode_VkImageSubresourceLayers(
	struct vulkan_writer *writer,
	const VkImageSubresourceLayers *record);

void
vulkan_decode_VkImageSubresourceLayers(
	struct vulkan_reader *reader,
	VkImageSubresourceLayers *record);

void
vulkan_encode_VkBufferImageCopy(
	struct vulkan_writer *writer,
	const VkBufferImageCopy *record);

void
vulkan_decode_VkBufferImageCopy(
	struct vulkan_reader *reader,
	VkBufferImageCopy *record);

void
vulkan_encode_VkClearDepthStencilValue(
	struct vulkan_writer *writer,
	const VkClearDepthStencilValue *record);

void
vulkan_decode_VkClearDepthStencilValue(
	struct vulkan_reader *reader,
	VkClearDepthStencilValue *record);

void
vulkan_encode_VkClearRect(
	struct vulkan_writer *writer,
	const VkClearRect *record);

void
vulkan_decode_VkClearRect(
	struct vulkan_reader *reader,
	VkClearRect *record);

void
vulkan_encode_VkImageBlit(
	struct vulkan_writer *writer,
	const VkImageBlit *record);

void
vulkan_decode_VkImageBlit(
	struct vulkan_reader *reader,
	VkImageBlit *record);

void
vulkan_encode_VkImageCopy(
	struct vulkan_writer *writer,
	const VkImageCopy *record);

void
vulkan_decode_VkImageCopy(
	struct vulkan_reader *reader,
	VkImageCopy *record);

void
vulkan_encode_VkImageResolve(
	struct vulkan_writer *writer,
	const VkImageResolve *record);

void
vulkan_decode_VkImageResolve(
	struct vulkan_reader *reader,
	VkImageResolve *record);

#endif
