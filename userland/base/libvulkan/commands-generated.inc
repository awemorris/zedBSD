/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Maintained Vulkan 1.0 recording wrappers, no scene or shader assumptions.
 */

/*
 * Records vkCmdBindPipeline with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(
	VkCommandBuffer commandBuffer,
	VkPipelineBindPoint pipelineBindPoint,
	VkPipeline pipeline)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBindPipeline);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, pipelineBindPoint);
	vulkan_encode_handle(&writer, (uint64_t)pipeline);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetViewport with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetViewport(
	VkCommandBuffer commandBuffer,
	uint32_t firstViewport,
	uint32_t viewportCount,
	const VkViewport* pViewports)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetViewport);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, firstViewport);
	vulkan_write_u32(&writer, viewportCount);

	/* Keeps absent pViewports distinct from its count-selected payload. */
	count = 0;
	if (pViewports != NULL)
		count = viewportCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkViewport(&writer, &pViewports[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetScissor with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetScissor(
	VkCommandBuffer commandBuffer,
	uint32_t firstScissor,
	uint32_t scissorCount,
	const VkRect2D* pScissors)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetScissor);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, firstScissor);
	vulkan_write_u32(&writer, scissorCount);

	/* Keeps absent pScissors distinct from its count-selected payload. */
	count = 0;
	if (pScissors != NULL)
		count = scissorCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkRect2D(&writer, &pScissors[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetLineWidth with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetLineWidth(
	VkCommandBuffer commandBuffer,
	float lineWidth)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetLineWidth);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_float(&writer, lineWidth);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetDepthBias with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetDepthBias(
	VkCommandBuffer commandBuffer,
	float depthBiasConstantFactor,
	float depthBiasClamp,
	float depthBiasSlopeFactor)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetDepthBias);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_float(&writer, depthBiasConstantFactor);
	vulkan_write_float(&writer, depthBiasClamp);
	vulkan_write_float(&writer, depthBiasSlopeFactor);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetBlendConstants with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetBlendConstants(
	VkCommandBuffer commandBuffer,
	const float blendConstants[4])
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetBlendConstants);
	if (!active)
		return;

	/* Keeps absent blendConstants distinct from its count-selected payload. */
	count = 0;
	if (blendConstants != NULL)
		count = 4;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_write_float(&writer, blendConstants[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetDepthBounds with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetDepthBounds(
	VkCommandBuffer commandBuffer,
	float minDepthBounds,
	float maxDepthBounds)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetDepthBounds);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_float(&writer, minDepthBounds);
	vulkan_write_float(&writer, maxDepthBounds);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetStencilCompareMask with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetStencilCompareMask(
	VkCommandBuffer commandBuffer,
	VkStencilFaceFlags faceMask,
	uint32_t compareMask)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetStencilCompareMask);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, faceMask);
	vulkan_write_u32(&writer, compareMask);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetStencilWriteMask with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetStencilWriteMask(
	VkCommandBuffer commandBuffer,
	VkStencilFaceFlags faceMask,
	uint32_t writeMask)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetStencilWriteMask);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, faceMask);
	vulkan_write_u32(&writer, writeMask);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetStencilReference with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetStencilReference(
	VkCommandBuffer commandBuffer,
	VkStencilFaceFlags faceMask,
	uint32_t reference)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetStencilReference);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, faceMask);
	vulkan_write_u32(&writer, reference);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdBindDescriptorSets with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(
	VkCommandBuffer commandBuffer,
	VkPipelineBindPoint pipelineBindPoint,
	VkPipelineLayout layout,
	uint32_t firstSet,
	uint32_t descriptorSetCount,
	const VkDescriptorSet* pDescriptorSets,
	uint32_t dynamicOffsetCount,
	const uint32_t* pDynamicOffsets)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBindDescriptorSets);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, pipelineBindPoint);
	vulkan_encode_handle(&writer, (uint64_t)layout);
	vulkan_write_u32(&writer, firstSet);
	vulkan_write_u32(&writer, descriptorSetCount);

	/* Keeps absent pDescriptorSets distinct from its count-selected payload. */
	count = 0;
	if (pDescriptorSets != NULL)
		count = descriptorSetCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_handle(&writer, (uint64_t)pDescriptorSets[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, dynamicOffsetCount);

	/* Keeps absent pDynamicOffsets distinct from its count-selected payload. */
	count = 0;
	if (pDynamicOffsets != NULL)
		count = dynamicOffsetCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_write_u32(&writer, pDynamicOffsets[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdBindIndexBuffer with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkIndexType indexType)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBindIndexBuffer);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)buffer);
	vulkan_write_u64(&writer, offset);
	vulkan_write_u32(&writer, indexType);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdBindVertexBuffers with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers(
	VkCommandBuffer commandBuffer,
	uint32_t firstBinding,
	uint32_t bindingCount,
	const VkBuffer* pBuffers,
	const VkDeviceSize* pOffsets)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBindVertexBuffers);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, firstBinding);
	vulkan_write_u32(&writer, bindingCount);

	/* Keeps absent pBuffers distinct from its count-selected payload. */
	count = 0;
	if (pBuffers != NULL)
		count = bindingCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_handle(&writer, (uint64_t)pBuffers[index]);
	}

	/* Keeps absent pOffsets distinct from its count-selected payload. */
	count = 0;
	if (pOffsets != NULL)
		count = bindingCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_write_u64(&writer, pOffsets[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdDraw with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDraw(
	VkCommandBuffer commandBuffer,
	uint32_t vertexCount,
	uint32_t instanceCount,
	uint32_t firstVertex,
	uint32_t firstInstance)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdDraw);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, vertexCount);
	vulkan_write_u32(&writer, instanceCount);
	vulkan_write_u32(&writer, firstVertex);
	vulkan_write_u32(&writer, firstInstance);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdDrawIndexed with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexed(
	VkCommandBuffer commandBuffer,
	uint32_t indexCount,
	uint32_t instanceCount,
	uint32_t firstIndex,
	int32_t vertexOffset,
	uint32_t firstInstance)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdDrawIndexed);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, indexCount);
	vulkan_write_u32(&writer, instanceCount);
	vulkan_write_u32(&writer, firstIndex);
	vulkan_write_u32(&writer, vertexOffset);
	vulkan_write_u32(&writer, firstInstance);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdDrawIndirect with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndirect(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkDeviceSize offset,
	uint32_t drawCount,
	uint32_t stride)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdDrawIndirect);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)buffer);
	vulkan_write_u64(&writer, offset);
	vulkan_write_u32(&writer, drawCount);
	vulkan_write_u32(&writer, stride);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdDrawIndexedIndirect with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexedIndirect(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkDeviceSize offset,
	uint32_t drawCount,
	uint32_t stride)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdDrawIndexedIndirect);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)buffer);
	vulkan_write_u64(&writer, offset);
	vulkan_write_u32(&writer, drawCount);
	vulkan_write_u32(&writer, stride);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdDispatch with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDispatch(
	VkCommandBuffer commandBuffer,
	uint32_t groupCountX,
	uint32_t groupCountY,
	uint32_t groupCountZ)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdDispatch);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, groupCountX);
	vulkan_write_u32(&writer, groupCountY);
	vulkan_write_u32(&writer, groupCountZ);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdDispatchIndirect with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDispatchIndirect(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkDeviceSize offset)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdDispatchIndirect);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)buffer);
	vulkan_write_u64(&writer, offset);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdCopyBuffer with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBuffer(
	VkCommandBuffer commandBuffer,
	VkBuffer srcBuffer,
	VkBuffer dstBuffer,
	uint32_t regionCount,
	const VkBufferCopy* pRegions)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdCopyBuffer);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)srcBuffer);
	vulkan_encode_handle(&writer, (uint64_t)dstBuffer);
	vulkan_write_u32(&writer, regionCount);

	/* Keeps absent pRegions distinct from its count-selected payload. */
	count = 0;
	if (pRegions != NULL)
		count = regionCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkBufferCopy(&writer, &pRegions[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdCopyImage with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImage(
	VkCommandBuffer commandBuffer,
	VkImage srcImage,
	VkImageLayout srcImageLayout,
	VkImage dstImage,
	VkImageLayout dstImageLayout,
	uint32_t regionCount,
	const VkImageCopy* pRegions)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdCopyImage);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)srcImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(srcImageLayout));
	vulkan_encode_handle(&writer, (uint64_t)dstImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(dstImageLayout));
	vulkan_write_u32(&writer, regionCount);

	/* Keeps absent pRegions distinct from its count-selected payload. */
	count = 0;
	if (pRegions != NULL)
		count = regionCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageCopy(&writer, &pRegions[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdBlitImage with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBlitImage(
	VkCommandBuffer commandBuffer,
	VkImage srcImage,
	VkImageLayout srcImageLayout,
	VkImage dstImage,
	VkImageLayout dstImageLayout,
	uint32_t regionCount,
	const VkImageBlit* pRegions,
	VkFilter filter)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBlitImage);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)srcImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(srcImageLayout));
	vulkan_encode_handle(&writer, (uint64_t)dstImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(dstImageLayout));
	vulkan_write_u32(&writer, regionCount);

	/* Keeps absent pRegions distinct from its count-selected payload. */
	count = 0;
	if (pRegions != NULL)
		count = regionCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageBlit(&writer, &pRegions[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, filter);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdCopyBufferToImage with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBufferToImage(
	VkCommandBuffer commandBuffer,
	VkBuffer srcBuffer,
	VkImage dstImage,
	VkImageLayout dstImageLayout,
	uint32_t regionCount,
	const VkBufferImageCopy* pRegions)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdCopyBufferToImage);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)srcBuffer);
	vulkan_encode_handle(&writer, (uint64_t)dstImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(dstImageLayout));
	vulkan_write_u32(&writer, regionCount);

	/* Keeps absent pRegions distinct from its count-selected payload. */
	count = 0;
	if (pRegions != NULL)
		count = regionCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkBufferImageCopy(&writer, &pRegions[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdCopyImageToBuffer with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer(
	VkCommandBuffer commandBuffer,
	VkImage srcImage,
	VkImageLayout srcImageLayout,
	VkBuffer dstBuffer,
	uint32_t regionCount,
	const VkBufferImageCopy* pRegions)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdCopyImageToBuffer);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)srcImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(srcImageLayout));
	vulkan_encode_handle(&writer, (uint64_t)dstBuffer);
	vulkan_write_u32(&writer, regionCount);

	/* Keeps absent pRegions distinct from its count-selected payload. */
	count = 0;
	if (pRegions != NULL)
		count = regionCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkBufferImageCopy(&writer, &pRegions[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdUpdateBuffer with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdUpdateBuffer(
	VkCommandBuffer commandBuffer,
	VkBuffer dstBuffer,
	VkDeviceSize dstOffset,
	VkDeviceSize dataSize,
	const void* pData)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdUpdateBuffer);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)dstBuffer);
	vulkan_write_u64(&writer, dstOffset);
	vulkan_write_u64(&writer, dataSize);

	/* Keeps absent pData distinct from its count-selected payload. */
	count = 0;
	if (pData != NULL)
		count = dataSize;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);
	vulkan_write_bytes(&writer, pData, (size_t)count);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdFillBuffer with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdFillBuffer(
	VkCommandBuffer commandBuffer,
	VkBuffer dstBuffer,
	VkDeviceSize dstOffset,
	VkDeviceSize size,
	uint32_t data)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdFillBuffer);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)dstBuffer);
	vulkan_write_u64(&writer, dstOffset);
	vulkan_write_u64(&writer, size);
	vulkan_write_u32(&writer, data);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdClearColorImage with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkImageLayout imageLayout,
	const VkClearColorValue* pColor,
	uint32_t rangeCount,
	const VkImageSubresourceRange* pRanges)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdClearColorImage);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)image);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(imageLayout));

	/* Keeps absent pColor distinct from its count-selected payload. */
	count = 0;
	if (pColor != NULL)
		count = 1;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Does not inspect a record whose presence marker is absent. */
	if (count != 0)
		command_encode_color(&writer, pColor);

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, rangeCount);

	/* Keeps absent pRanges distinct from its count-selected payload. */
	count = 0;
	if (pRanges != NULL)
		count = rangeCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageSubresourceRange(&writer, &pRanges[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdClearDepthStencilImage with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdClearDepthStencilImage(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkImageLayout imageLayout,
	const VkClearDepthStencilValue* pDepthStencil,
	uint32_t rangeCount,
	const VkImageSubresourceRange* pRanges)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdClearDepthStencilImage);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)image);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(imageLayout));

	/* Keeps absent pDepthStencil distinct from its count-selected payload. */
	count = 0;
	if (pDepthStencil != NULL)
		count = 1;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Does not inspect a record whose presence marker is absent. */
	if (count != 0)
		command_encode_depth_ranges(&writer, pDepthStencil, rangeCount, pRanges);

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, rangeCount);

	/* Keeps absent pRanges distinct from its count-selected payload. */
	count = 0;
	if (pRanges != NULL)
		count = rangeCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageSubresourceRange(&writer, &pRanges[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdClearAttachments with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdClearAttachments(
	VkCommandBuffer commandBuffer,
	uint32_t attachmentCount,
	const VkClearAttachment* pAttachments,
	uint32_t rectCount,
	const VkClearRect* pRects)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdClearAttachments);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, attachmentCount);

	/* Keeps absent pAttachments distinct from its count-selected payload. */
	count = 0;
	if (pAttachments != NULL)
		count = attachmentCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		command_encode_clear_attachment(&writer, &pAttachments[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, rectCount);

	/* Keeps absent pRects distinct from its count-selected payload. */
	count = 0;
	if (pRects != NULL)
		count = rectCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkClearRect(&writer, &pRects[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdResolveImage with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdResolveImage(
	VkCommandBuffer commandBuffer,
	VkImage srcImage,
	VkImageLayout srcImageLayout,
	VkImage dstImage,
	VkImageLayout dstImageLayout,
	uint32_t regionCount,
	const VkImageResolve* pRegions)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdResolveImage);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)srcImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(srcImageLayout));
	vulkan_encode_handle(&writer, (uint64_t)dstImage);
	vulkan_write_u32(&writer, vulkan_wire_image_layout(dstImageLayout));
	vulkan_write_u32(&writer, regionCount);

	/* Keeps absent pRegions distinct from its count-selected payload. */
	count = 0;
	if (pRegions != NULL)
		count = regionCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageResolve(&writer, &pRegions[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdSetEvent with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetEvent(
	VkCommandBuffer commandBuffer,
	VkEvent event,
	VkPipelineStageFlags stageMask)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdSetEvent);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)event);
	vulkan_write_u32(&writer, stageMask);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdResetEvent with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdResetEvent(
	VkCommandBuffer commandBuffer,
	VkEvent event,
	VkPipelineStageFlags stageMask)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdResetEvent);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)event);
	vulkan_write_u32(&writer, stageMask);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdWaitEvents with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdWaitEvents(
	VkCommandBuffer commandBuffer,
	uint32_t eventCount,
	const VkEvent* pEvents,
	VkPipelineStageFlags srcStageMask,
	VkPipelineStageFlags dstStageMask,
	uint32_t memoryBarrierCount,
	const VkMemoryBarrier* pMemoryBarriers,
	uint32_t bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier* pBufferMemoryBarriers,
	uint32_t imageMemoryBarrierCount,
	const VkImageMemoryBarrier* pImageMemoryBarriers)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdWaitEvents);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, eventCount);

	/* Keeps absent pEvents distinct from its count-selected payload. */
	count = 0;
	if (pEvents != NULL)
		count = eventCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_handle(&writer, (uint64_t)pEvents[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, srcStageMask);
	vulkan_write_u32(&writer, dstStageMask);
	vulkan_write_u32(&writer, memoryBarrierCount);

	/* Keeps absent pMemoryBarriers distinct from its count-selected payload. */
	count = 0;
	if (pMemoryBarriers != NULL)
		count = memoryBarrierCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkMemoryBarrier(&writer, &pMemoryBarriers[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, bufferMemoryBarrierCount);

	/* Keeps absent pBufferMemoryBarriers distinct from its count-selected payload. */
	count = 0;
	if (pBufferMemoryBarriers != NULL)
		count = bufferMemoryBarrierCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkBufferMemoryBarrier(&writer, &pBufferMemoryBarriers[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, imageMemoryBarrierCount);

	/* Keeps absent pImageMemoryBarriers distinct from its count-selected payload. */
	count = 0;
	if (pImageMemoryBarriers != NULL)
		count = imageMemoryBarrierCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageMemoryBarrier(&writer, &pImageMemoryBarriers[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdPipelineBarrier with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(
	VkCommandBuffer commandBuffer,
	VkPipelineStageFlags srcStageMask,
	VkPipelineStageFlags dstStageMask,
	VkDependencyFlags dependencyFlags,
	uint32_t memoryBarrierCount,
	const VkMemoryBarrier* pMemoryBarriers,
	uint32_t bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier* pBufferMemoryBarriers,
	uint32_t imageMemoryBarrierCount,
	const VkImageMemoryBarrier* pImageMemoryBarriers)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdPipelineBarrier);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, srcStageMask);
	vulkan_write_u32(&writer, dstStageMask);
	vulkan_write_u32(&writer, dependencyFlags);
	vulkan_write_u32(&writer, memoryBarrierCount);

	/* Keeps absent pMemoryBarriers distinct from its count-selected payload. */
	count = 0;
	if (pMemoryBarriers != NULL)
		count = memoryBarrierCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkMemoryBarrier(&writer, &pMemoryBarriers[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, bufferMemoryBarrierCount);

	/* Keeps absent pBufferMemoryBarriers distinct from its count-selected payload. */
	count = 0;
	if (pBufferMemoryBarriers != NULL)
		count = bufferMemoryBarrierCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkBufferMemoryBarrier(&writer, &pBufferMemoryBarriers[index]);
	}

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, imageMemoryBarrierCount);

	/* Keeps absent pImageMemoryBarriers distinct from its count-selected payload. */
	count = 0;
	if (pImageMemoryBarriers != NULL)
		count = imageMemoryBarrierCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_VkImageMemoryBarrier(&writer, &pImageMemoryBarriers[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdBeginQuery with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBeginQuery(
	VkCommandBuffer commandBuffer,
	VkQueryPool queryPool,
	uint32_t query,
	VkQueryControlFlags flags)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBeginQuery);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)queryPool);
	vulkan_write_u32(&writer, query);
	vulkan_write_u32(&writer, flags);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdEndQuery with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdEndQuery(
	VkCommandBuffer commandBuffer,
	VkQueryPool queryPool,
	uint32_t query)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdEndQuery);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)queryPool);
	vulkan_write_u32(&writer, query);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdResetQueryPool with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdResetQueryPool(
	VkCommandBuffer commandBuffer,
	VkQueryPool queryPool,
	uint32_t firstQuery,
	uint32_t queryCount)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdResetQueryPool);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)queryPool);
	vulkan_write_u32(&writer, firstQuery);
	vulkan_write_u32(&writer, queryCount);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdWriteTimestamp with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp(
	VkCommandBuffer commandBuffer,
	VkPipelineStageFlagBits pipelineStage,
	VkQueryPool queryPool,
	uint32_t query)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdWriteTimestamp);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, pipelineStage);
	vulkan_encode_handle(&writer, (uint64_t)queryPool);
	vulkan_write_u32(&writer, query);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdCopyQueryPoolResults with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyQueryPoolResults(
	VkCommandBuffer commandBuffer,
	VkQueryPool queryPool,
	uint32_t firstQuery,
	uint32_t queryCount,
	VkBuffer dstBuffer,
	VkDeviceSize dstOffset,
	VkDeviceSize stride,
	VkQueryResultFlags flags)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdCopyQueryPoolResults);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)queryPool);
	vulkan_write_u32(&writer, firstQuery);
	vulkan_write_u32(&writer, queryCount);
	vulkan_encode_handle(&writer, (uint64_t)dstBuffer);
	vulkan_write_u64(&writer, dstOffset);
	vulkan_write_u64(&writer, stride);
	vulkan_write_u32(&writer, flags);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdPushConstants with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdPushConstants(
	VkCommandBuffer commandBuffer,
	VkPipelineLayout layout,
	VkShaderStageFlags stageFlags,
	uint32_t offset,
	uint32_t size,
	const void* pValues)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdPushConstants);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_encode_handle(&writer, (uint64_t)layout);
	vulkan_write_u32(&writer, stageFlags);
	vulkan_write_u32(&writer, offset);
	vulkan_write_u32(&writer, size);

	/* Keeps absent pValues distinct from its count-selected payload. */
	count = 0;
	if (pValues != NULL)
		count = size;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);
	vulkan_write_bytes(&writer, pValues, (size_t)count);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdBeginRenderPass with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderPass(
	VkCommandBuffer commandBuffer,
	const VkRenderPassBeginInfo* pRenderPassBegin,
	VkSubpassContents contents)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdBeginRenderPass);
	if (!active)
		return;

	/* Keeps absent pRenderPassBegin distinct from its count-selected payload. */
	count = 0;
	if (pRenderPassBegin != NULL)
		count = 1;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Does not inspect a record whose presence marker is absent. */
	if (count != 0)
		command_encode_render_begin(&writer, pRenderPassBegin);

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, contents);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdNextSubpass with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdNextSubpass(
	VkCommandBuffer commandBuffer,
	VkSubpassContents contents)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdNextSubpass);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, contents);

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdEndRenderPass with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdEndRenderPass(
	VkCommandBuffer commandBuffer)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdEndRenderPass);
	if (!active)
		return;

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}

/*
 * Records vkCmdExecuteCommands with the caller's standard parameters and owned object identities.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdExecuteCommands(
	VkCommandBuffer commandBuffer,
	uint32_t commandBufferCount,
	const VkCommandBuffer* pCommandBuffers)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	VkBool32 active;
	uint32_t index;
	uint64_t count;

	/* Preserves the first recording failure for vkEndCommandBuffer to report. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	active = command_record_begin(command, &writer, VULKAN_OPCODE_vkCmdExecuteCommands);
	if (!active)
		return;

	/* Preserves argument order with explicit scalar widths and owned handle translation. */
	vulkan_write_u32(&writer, commandBufferCount);

	/* Keeps absent pCommandBuffers distinct from its count-selected payload. */
	count = 0;
	if (pCommandBuffers != NULL)
		count = commandBufferCount;

	/* Prefixes referenced contents with their exact 64-bit wire extent. */
	vulkan_write_u64(&writer, count);

	/* Copies only complete caller elements while the command remains encodable. */
	for (index = 0;
	     index < count && writer.error == VK_SUCCESS;
	     index++) {
		/* Uses declared element widths rather than native array padding. */
		vulkan_encode_handle(&writer, (uint64_t)(uintptr_t)pCommandBuffers[index]);
	}

	/* Native recording completion is distinct from later GPU execution completion. */
	command_record_finish(command, &writer);

	/* Succeeded: the command is recorded or its first error is retained by the command buffer. */
	return;
}
