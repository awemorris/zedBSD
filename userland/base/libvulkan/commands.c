/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Records arbitrary Vulkan 1.0 command streams with ordinary pool ownership.
 */

#include <string.h>
#include "internal.h"

/* Tracks a command pool's implicit command-buffer lifetime and native queue family. */
struct vulkan_command_pool {
	struct vulkan_object object;
	struct VkDevice_T *device;
	VkCommandPoolCreateFlags flags;
	uint32_t family;
};

/* Recording failures remain local until the standard result-bearing end/reset calls. */
enum vulkan_command_state {
	VULKAN_COMMAND_INITIAL,
	VULKAN_COMMAND_RECORDING,
	VULKAN_COMMAND_EXECUTABLE,
	VULKAN_COMMAND_INVALID
};

static VkBool32 command_record_begin(struct VkCommandBuffer_T *command, struct vulkan_writer *writer, uint32_t opcode);
static void command_record_finish(struct VkCommandBuffer_T *command, struct vulkan_writer *writer);
static void command_encode_color(struct vulkan_writer *writer, const VkClearColorValue *color);
static void command_encode_depth_ranges(struct vulkan_writer *writer, const VkClearDepthStencilValue *clear, uint32_t count, const VkImageSubresourceRange *ranges);
static void command_encode_clear_attachment(struct vulkan_writer *writer, const VkClearAttachment *attachment);
static void command_encode_render_begin(struct vulkan_writer *writer, const VkRenderPassBeginInfo *info);
static void command_encode_render_clear(struct vulkan_writer *writer, const VkAttachmentDescription *attachment, const VkClearValue *clear);
static void command_buffers_local_free(uint32_t count, VkCommandBuffer *buffers);
static void command_buffer_initial(struct VkCommandBuffer_T *command);

/*
 * Creates a normal command pool with the requested queue family and allocation flags.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(
	VkDevice device,
	const VkCommandPoolCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkCommandPool *pCommandPool)
{
	struct VkDevice_T *owner;
	struct vulkan_command_pool *pool;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Preallocates pool ownership using the application's effective allocation policy. */
	owner = vulkan_device(device);
	status = vulkan_object_alloc(sizeof(*pool), __alignof__(struct vulkan_command_pool), VULKAN_OBJECT_COMMAND_POOL, &owner->object, owner->object.context, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Keeps pool metadata and its protocol identity ready before requesting native creation. */
	pool = (struct vulkan_command_pool *)object;
	pool->device = owner;
	pool->flags = pCreateInfo->flags;
	pool->family = pCreateInfo->queueFamilyIndex;
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Encodes the complete standard pool creation record with no scene-specific defaults. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateCommandPool);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandPoolCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyCommandPool);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Publishes the standard opaque handle only after native creation and local ownership succeeded. */
	handle = vulkan_nondispatchable_handle(object);
	*pCommandPool = (VkCommandPool)(uintptr_t)handle;

	/* Succeeded: subsequent allocations use this ordinary command pool. */
	return VK_SUCCESS;
}

/*
 * Destroys a pool and implicitly frees every command buffer allocated from it.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(
	VkDevice device,
	VkCommandPool commandPool,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_command_pool *pool;
	struct vulkan_object *child;
	VkResult status;

	/* Null pool destruction has no implicit child ownership to consume. */
	owner = vulkan_device(device);
	pool = (struct vulkan_command_pool *)vulkan_nondispatchable_object((uint64_t)commandPool);
	if (pool == NULL)
		return;

	/* All pool and pooled-object allocations use this command's compatible policy. */
	if (pAllocator != NULL) {
		pool->object.allocator.callbacks = *pAllocator;
		pool->object.allocator.has_callbacks = VK_TRUE;
	}

	/* Native pool destruction consumes its native command buffers in the same operation. */
	status = vulkan_object_destroy_remote(owner, &pool->object, VULKAN_OPCODE_vkDestroyCommandPool);
	if (status != VK_SUCCESS)
		__atomic_store_n(&pool->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* The application's external pool synchronization makes this child traversal exclusive. */
	while (pool->object.first_child != NULL) {
		child = pool->object.first_child;
		child->allocator = pool->object.allocator;
		vulkan_object_free(child);
	}

	/* Returns the pool itself only after all implicitly owned local identities are gone. */
	vulkan_object_free(&pool->object);

	/* Succeeded: the pool and its command buffers no longer own local or native resources. */
	return;
}

/*
 * Resets the native pool and every local command buffer only after native success.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandPool(
	VkDevice device,
	VkCommandPool commandPool,
	VkCommandPoolResetFlags flags)
{
	struct VkDevice_T *owner;
	struct vulkan_command_pool *pool;
	struct vulkan_object *child;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Preserves the caller's release-resources flag while addressing the ordinary native pool. */
	owner = vulkan_device(device);
	pool = (struct vulkan_command_pool *)vulkan_nondispatchable_object((uint64_t)commandPool);
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkResetCommandPool);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, pool->object.wire_id);
	vulkan_write_u32(&writer, flags);
	status = vulkan_command_execute(owner->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Resets local recording errors only when the native pool reset actually completed. */
	child = pool->object.first_child;
	while (child != NULL) {
		command_buffer_initial((struct VkCommandBuffer_T *)child);
		child = child->next_sibling;
	}

	/* Succeeded: every command buffer from this pool is back in its initial state. */
	return VK_SUCCESS;
}

/*
 * Allocates a dynamically sized batch of primary or secondary command buffers.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(
	VkDevice device,
	const VkCommandBufferAllocateInfo *pAllocateInfo,
	VkCommandBuffer *pCommandBuffers)
{
	struct VkDevice_T *owner;
	struct vulkan_command_pool *pool;
	struct VkCommandBuffer_T *command;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkCommandBuffer *buffers;
	uint32_t count;
	uint32_t index;
	uint64_t returned;
	uint64_t identity;
	size_t bytes;
	VkResult status;

	/* Failed allocation returns null for every requested output command buffer. */
	owner = vulkan_device(device);
	pool = (struct vulkan_command_pool *)vulkan_nondispatchable_object((uint64_t)pAllocateInfo->commandPool);
	count = pAllocateInfo->commandBufferCount;
	for (index = 0; index < count; index++) {
		pCommandBuffers[index] = VK_NULL_HANDLE;
	}

	/* Checks pointer-array and reply sizes before any native command buffers can exist. */
	bytes = (size_t)count * sizeof(*buffers);
	if (bytes / sizeof(*buffers) != count || (uint64_t)count * 8 + 16 > SIZE_MAX)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Temporary handle bookkeeping lasts only for this allocation command. */
	buffers = vulkan_allocate(&pool->object.allocator, bytes, __alignof__(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (buffers == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Zeroed private slots permit complete rollback after any local allocation failure. */
	memset(buffers, 0, bytes);
	status = VK_SUCCESS;
	for (index = 0; index < count; index++) {
		status = vulkan_object_alloc(sizeof(*command), __alignof__(struct VkCommandBuffer_T), VULKAN_OBJECT_COMMAND_BUFFER, &pool->object, pool->object.context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
		if (status != VK_SUCCESS)
			break;

		/* Records the pool and level before assigning a never-reused native identity. */
		command = (struct VkCommandBuffer_T *)object;
		command->pool = pool;
		command->level = pAllocateInfo->level;
		buffers[index] = (VkCommandBuffer)command;
		status = vulkan_object_reserve_id(object);
		if (status != VK_SUCCESS)
			break;
	}

	/* Pre-native failure consumes only private host allocations. */
	if (status != VK_SUCCESS) {
		command_buffers_local_free(count, buffers);
		vulkan_free(&pool->object.allocator, buffers);
		return status;
	}

	/* Sends the exact pool, level, and output identities for this dynamic batch. */
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkAllocateCommandBuffers);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&writer, pAllocateInfo);
	vulkan_write_u64(&writer, count);
	for (index = 0; index < count; index++) {
		command = (struct VkCommandBuffer_T *)buffers[index];
		vulkan_write_u64(&writer, command->object.wire_id);
	}

	/* The native API either allocates the whole batch or frees all partial native allocations. */
	status = vulkan_command_execute(owner->object.context, &writer, (size_t)count * 8 + 16, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status == VK_SUCCESS) {
		/* Requires exactly this batch count before accepting any returned identity. */
		returned = vulkan_read_u64(&reader);
		if (returned != count)
			reader.error = VK_ERROR_DEVICE_LOST;

		/* Rejects foreign native identity mappings before exposing caller handles. */
		for (index = 0;
		     index < count && reader.error == VK_SUCCESS;
		     index++) {
			command = (struct VkCommandBuffer_T *)buffers[index];
			identity = vulkan_read_u64(&reader);
			if (identity != command->object.wire_id)
				reader.error = VK_ERROR_DEVICE_LOST;
		}
	}

	/* No unsuccessful batch leaves a reachable local command buffer. */
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS) {
		command_buffers_local_free(count, buffers);
		vulkan_free(&pool->object.allocator, buffers);
		return status;
	}

	/* Publishes all locally complete objects after native allocation has succeeded. */
	for (index = 0; index < count; index++) {
		command = (struct VkCommandBuffer_T *)buffers[index];
		status = vulkan_object_publish(&command->object);
		if (status != VK_SUCCESS) {
			vkFreeCommandBuffers(device, pAllocateInfo->commandPool, count, buffers);
			vulkan_free(&pool->object.allocator, buffers);
			return status;
		}
	}

	/* Transfers the complete batch to caller output before releasing temporary bookkeeping. */
	memcpy(pCommandBuffers, buffers, bytes);
	vulkan_free(&pool->object.allocator, buffers);

	/* Succeeded: every returned buffer belongs to this pool at the requested level. */
	return VK_SUCCESS;
}

/*
 * Frees selected command buffers without disturbing other allocations from the pool.
 */
VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers(
	VkDevice device,
	VkCommandPool commandPool,
	uint32_t commandBufferCount,
	const VkCommandBuffer *pCommandBuffers)
{
	struct VkDevice_T *owner;
	struct vulkan_command_pool *pool;
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint32_t index;
	VkResult status;

	/* Native free consumes exactly the selected command-buffer array, including allowed nulls. */
	owner = vulkan_device(device);
	pool = (struct vulkan_command_pool *)vulkan_nondispatchable_object((uint64_t)commandPool);
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkFreeCommandBuffers);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, pool->object.wire_id);
	vulkan_write_u32(&writer, commandBufferCount);
	vulkan_write_u64(&writer, commandBufferCount);
	for (index = 0; index < commandBufferCount; index++) {
		vulkan_encode_handle(&writer, (uint64_t)(uintptr_t)pCommandBuffers[index]);
	}

	/* A failed native free must not let surviving local commands reuse uncertain native ownership. */
	status = vulkan_command_execute(owner->object.context, &writer, 4, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		__atomic_store_n(&owner->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* Releases only the requested local objects after native consumption or terminal session loss. */
	for (index = 0; index < commandBufferCount; index++) {
		command = (struct VkCommandBuffer_T *)pCommandBuffers[index];
		if (command != NULL) {
			command->object.allocator = pool->object.allocator;
			vulkan_object_free(&command->object);
		}
	}

	/* Succeeded: every non-null selected handle has been consumed exactly once. */
	return;
}

/*
 * Begins native command recording with inheritance fields active for the selected level.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(
	VkCommandBuffer commandBuffer,
	const VkCommandBufferBeginInfo *pBeginInfo)
{
	struct VkCommandBuffer_T *command;
	VkCommandBufferBeginInfo begin;
	VkCommandBufferInheritanceInfo inheritance;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Primary buffers never dereference their ignored inheritance pointer. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	begin = *pBeginInfo;
	begin.pNext = NULL;
	begin.pInheritanceInfo = NULL;
	if (command->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
		/* Secondary buffers retain query inheritance even outside a render-pass continuation. */
		inheritance = *pBeginInfo->pInheritanceInfo;
		inheritance.pNext = NULL;
		begin.pInheritanceInfo = &inheritance;

		/* Non-continuation secondary buffers must not dereference ignored render-pass handles. */
		if (!(begin.flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
			inheritance.renderPass = VK_NULL_HANDLE;
			inheritance.framebuffer = VK_NULL_HANDLE;
			inheritance.subpass = 0;
		}

		/* Query control flags have no meaning without inherited occlusion queries. */
		if (!inheritance.occlusionQueryEnable)
			inheritance.queryFlags = 0;
	}

	/* Native Begin applies the standard initial/re-recording transition and pool flags. */
	vulkan_writer_init_for_object(&writer, &command->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkBeginCommandBuffer);
	vulkan_write_u64(&writer, command->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&writer, &begin);
	status = vulkan_command_execute(command->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(command->object.context, &reader, status);
	if (status != VK_SUCCESS) {
		command->error = status;
		command->state = VULKAN_COMMAND_INVALID;
		return status;
	}

	/* Clears the previous recording failure only after native Begin succeeds. */
	command->error = VK_SUCCESS;
	command->state = VULKAN_COMMAND_RECORDING;

	/* Succeeded: following standard recording calls target this active native command buffer. */
	return VK_SUCCESS;
}

/*
 * Ends native recording and reports the first failure retained by any void recording call.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(
	VkCommandBuffer commandBuffer)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult prior;
	VkResult status;

	/* Finishes native recording even after a local encoding failure so later reset/re-record is valid. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	prior = command->error;
	vulkan_writer_init_for_object(&writer, &command->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkEndCommandBuffer);
	vulkan_write_u64(&writer, command->object.wire_id);
	status = vulkan_command_execute(command->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(command->object.context, &reader, status);

	/* Preserves the first recording failure rather than claiming a partial buffer is executable. */
	if (prior != VK_SUCCESS)
		status = prior;

	/* Prevents a partially recorded native buffer from becoming locally executable. */
	if (status != VK_SUCCESS) {
		command->error = status;
		command->state = VULKAN_COMMAND_INVALID;
		return status;
	}

	/* A fully recorded native buffer becomes executable only on successful End. */
	command->state = VULKAN_COMMAND_EXECUTABLE;

	/* Succeeded: the caller may submit this complete standard command buffer. */
	return VK_SUCCESS;
}

/*
 * Resets one native command buffer and clears its retained local recording error.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandBuffer(
	VkCommandBuffer commandBuffer,
	VkCommandBufferResetFlags flags)
{
	struct VkCommandBuffer_T *command;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Preserves the requested resource-release behavior in the actual native reset. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	vulkan_writer_init_for_object(&writer, &command->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkResetCommandBuffer);
	vulkan_write_u64(&writer, command->object.wire_id);
	vulkan_write_u32(&writer, flags);
	status = vulkan_command_execute(command->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(command->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Local initial state follows successful native reset rather than preceding it. */
	command_buffer_initial(command);

	/* Succeeded: this command buffer may be recorded again under its pool's normal rules. */
	return VK_SUCCESS;
}

#include "commands-generated.inc"

/* Starts one ordinary recording call without losing an earlier encoding failure. */
static VkBool32
command_record_begin(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer,
	uint32_t opcode)
{
	/* Later void calls cannot clear or hide an earlier command-buffer failure. */
	if (command->error != VK_SUCCESS)
		return VK_FALSE;

	/* Invalid recording state never submits a command to an unrelated native operation. */
	if (command->state != VULKAN_COMMAND_RECORDING) {
		command->error = VK_ERROR_INITIALIZATION_FAILED;
		command->state = VULKAN_COMMAND_INVALID;
		return VK_FALSE;
	}

	/* Each public call owns its own independent command bytes and callback scope. */
	vulkan_writer_init_for_object(writer, &command->object);
	vulkan_command_begin(writer, opcode);
	vulkan_write_u64(writer, command->object.wire_id);

	/* Succeeded: typed argument encoders may append to this recording operation. */
	return VK_TRUE;
}

/* Executes one native recording call and retains any failure for standard End semantics. */
static void
command_record_finish(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer)
{
	struct vulkan_reader reader;
	VkResult status;

	/* Native reply completion proves recording, not execution or presentation completion. */
	status = vulkan_command_execute(command->object.context, writer, 4, &reader, VK_FALSE);
	vulkan_writer_finish(writer);
	status = vulkan_reply_finish(command->object.context, &reader, status);
	if (status != VK_SUCCESS) {
		command->error = status;
		command->state = VULKAN_COMMAND_INVALID;
		return;
	}

	/* Succeeded: the native buffer contains the complete requested recording operation. */
	return;
}

/* Encodes a color union by preserving its bits through the protocol's unsigned variant. */
static void
command_encode_color(
	struct vulkan_writer *writer,
	const VkClearColorValue *color)
{
	uint32_t words[4];
	uint32_t index;

	/* A bit-preserving copy supports float, signed, and unsigned clear values equally. */
	memcpy(words, color, sizeof(words));
	vulkan_write_u32(writer, 2);
	vulkan_write_u64(writer, 4);
	for (index = 0; index < 4; index++) {
		vulkan_write_u32(writer, words[index]);
	}

	/* Succeeded: native format interpretation sees the application's exact clear-value bits. */
	return;
}

/* Encodes only the depth/stencil members used by the requested clear subresource aspects. */
static void
command_encode_depth_ranges(
	struct vulkan_writer *writer,
	const VkClearDepthStencilValue *clear,
	uint32_t count,
	const VkImageSubresourceRange *ranges)
{
	VkImageAspectFlags aspects;
	float depth;
	uint32_t stencil;
	uint32_t index;

	/* Derives active union members from the complete standard subresource-range list. */
	aspects = 0;
	for (index = 0; index < count; index++) {
		aspects |= ranges[index].aspectMask;
	}

	/* Ignored clear members use deterministic values without reading application storage. */
	depth = 0.0f;
	stencil = 0;
	if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
		depth = clear->depth;

	/* A stencil-only clear never requires a readable depth member. */
	if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
		stencil = clear->stencil;

	/* These fields belong to VkClearDepthStencilValue directly, with no outer union tag. */
	vulkan_write_float(writer, depth);
	vulkan_write_u32(writer, stencil);

	/* Succeeded: only the aspects actually selected by the command affect native clear input. */
	return;
}

/* Selects the correct tagged clear-value payload from a standard clear attachment's aspects. */
static void
command_encode_clear_attachment(
	struct vulkan_writer *writer,
	const VkClearAttachment *attachment)
{
	float depth;
	uint32_t stencil;

	/* Color clears retain their attachment index and full bit-preserving color union. */
	vulkan_write_u32(writer, attachment->aspectMask);
	if (attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		vulkan_write_u32(writer, attachment->colorAttachment);
		vulkan_write_u32(writer, 0);
		command_encode_color(writer, &attachment->clearValue.color);
		return;
	}

	/* The color attachment index is ignored for a depth/stencil clear. */
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 1);
	depth = 0.0f;
	stencil = 0;
	if (attachment->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
		depth = attachment->clearValue.depthStencil.depth;

	/* A depth-only attachment does not supply a meaningful stencil member. */
	if (attachment->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
		stencil = attachment->clearValue.depthStencil.stencil;

	/* Encodes the active depth/stencil members without reading ignored color-union bytes. */
	vulkan_write_float(writer, depth);
	vulkan_write_u32(writer, stencil);

	/* Succeeded: the native attachment clear has a type-correct tagged payload. */
	return;
}

/* Encodes render-pass begin using attachment metadata to interpret only meaningful clear values. */
static void
command_encode_render_begin(
	struct vulkan_writer *writer,
	const VkRenderPassBeginInfo *info)
{
	const VkAttachmentDescription *attachment;
	const VkClearValue *clear;
	uint32_t index;

	/* Preserves the standard render area and ordinary object identities in protocol order. */
	vulkan_write_u32(writer, info->sType);
	vulkan_write_u64(writer, 0);
	vulkan_encode_handle(writer, (uint64_t)info->renderPass);
	vulkan_encode_handle(writer, (uint64_t)info->framebuffer);
	vulkan_encode_VkRect2D(writer, &info->renderArea);
	vulkan_write_u32(writer, info->clearValueCount);
	vulkan_write_u64(writer, info->clearValueCount);

	/* A valid begin may contain ignored clear entries beyond the last clear attachment. */
	for (index = 0;
	     index < info->clearValueCount && writer->error == VK_SUCCESS;
	     index++) {
		attachment = vulkan_render_pass_attachment(info->renderPass, index);
		clear = NULL;

		/* Defers caller clear storage access until a real attachment can make it meaningful. */
		if (attachment != NULL)
			clear = &info->pClearValues[index];
		command_encode_render_clear(writer, attachment, clear);
	}

	/* Succeeded: native clear decoding does not depend on unused or uninitialized union members. */
	return;
}

/* Encodes a render-pass attachment's active color/depth/stencil clear union. */
static void
command_encode_render_clear(
	struct vulkan_writer *writer,
	const VkAttachmentDescription *attachment,
	const VkClearValue *clear)
{
	VkClearColorValue empty;
	VkBool32 depth_format;
	VkBool32 stencil_format;
	float depth;
	uint32_t stencil;

	/* Unknown or ignored attachment entries have no application clear-value contents to read. */
	memset(&empty, 0, sizeof(empty));
	if (attachment == NULL) {
		vulkan_write_u32(writer, 0);
		command_encode_color(writer, &empty);
		return;
	}

	/* Classifies only standard depth/stencil formats; every other valid format is color. */
	depth_format = VK_FALSE;
	stencil_format = VK_FALSE;
	switch (attachment->format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
		depth_format = VK_TRUE;
		break;
	case VK_FORMAT_S8_UINT:
		stencil_format = VK_TRUE;
		break;
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		depth_format = VK_TRUE;
		stencil_format = VK_TRUE;
		break;
	default:
		break;
	}

	/* Color attachment load operations determine whether its union value is meaningful. */
	if (!depth_format && !stencil_format) {
		vulkan_write_u32(writer, 0);

		/* Reads the color union only when this attachment will actually clear. */
		if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
			command_encode_color(writer, &clear->color);
		} else {
			command_encode_color(writer, &empty);
		}

		/* Succeeded: this color attachment needs no depth or stencil payload. */
		return;
	}

	/* Each depth/stencil aspect has its own independent load-operation selection. */
	depth = 0.0f;
	stencil = 0;
	if (depth_format && attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
		depth = clear->depthStencil.depth;

	/* Stencil load selection is independent of the depth aspect in combined formats. */
	if (stencil_format && attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
		stencil = clear->depthStencil.stencil;

	/* The depth/stencil branch contains no color-array tag or unused color words. */
	vulkan_write_u32(writer, 1);
	vulkan_write_float(writer, depth);
	vulkan_write_u32(writer, stencil);

	/* Succeeded: every native clear member corresponds to an actual CLEAR load operation. */
	return;
}

/* Releases an unexposed allocation batch without issuing a native command. */
static void
command_buffers_local_free(
	uint32_t count,
	VkCommandBuffer *buffers)
{
	struct VkCommandBuffer_T *command;
	uint32_t index;

	/* Unallocated or already-null private slots have no local object ownership. */
	for (index = 0; index < count; index++) {
		command = (struct VkCommandBuffer_T *)buffers[index];
		if (command != NULL)
			vulkan_object_free(&command->object);
	}

	/* Succeeded: no unpublished command buffer remains allocated in the failed batch. */
	return;
}

/* Applies the local state guaranteed by a successful native allocation or reset. */
static void
command_buffer_initial(
	struct VkCommandBuffer_T *command)
{
	/* Recording errors belong to the discarded old recording and cannot survive a successful reset. */
	command->error = VK_SUCCESS;
	command->state = VULKAN_COMMAND_INITIAL;

	/* Succeeded: this command buffer is ready for a fresh standard Begin operation. */
	return;
}
