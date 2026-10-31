/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Records arbitrary Vulkan 1.0 command streams with ordinary pool ownership.
 */

#include <pthread.h>
#include <string.h>
#include "internal.h"

#define VULKAN_RECORDING_BATCH_BYTES (1024U * 1024U)

/*
 * The recording thread: it sends a long recording's batches, in the order
 * they were handed to it, while the application records the next one, so
 * the renderer's decoding of a batch and the encoding of the next run side
 * by side.  Its lock guards the queue of waiting batches (first and last),
 * each command buffer's sent_pending and sent_error and a batch's place
 * while it waits; ready wakes the thread, done wakes a command buffer
 * waiting for its batches.  started says whether the thread runs (it is
 * started once, by the first long recording; when it cannot be, batches
 * are sent by the recording call as before).
 */
static pthread_mutex_t command_sender_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t command_sender_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t command_sender_done = PTHREAD_COND_INITIALIZER;
static struct vulkan_recording_batch *command_sender_first;
static struct vulkan_recording_batch *command_sender_last;
static pthread_once_t command_sender_once = PTHREAD_ONCE_INIT;
static VkBool32 command_sender_started;

/* Recording failures remain local until the standard result-bearing end/reset calls. */
enum vulkan_command_state {
	VULKAN_COMMAND_INITIAL,
	VULKAN_COMMAND_RECORDING,
	VULKAN_COMMAND_EXECUTABLE,
	VULKAN_COMMAND_INVALID
};

/* Tracks a command pool's implicit command-buffer lifetime and native queue family. */
struct vulkan_command_pool {
	struct vulkan_object object;
	struct VkDevice_T *device;
	VkCommandPoolCreateFlags flags;
	uint32_t family;
};


static VkBool32 command_record_begin(struct VkCommandBuffer_T *command, struct vulkan_writer *writer, uint32_t opcode);
static void command_record_finish(struct VkCommandBuffer_T *command, struct vulkan_writer *writer);
static void command_head_add(struct VkCommandBuffer_T *command, uint32_t opcode);
static VkResult command_send(struct VkCommandBuffer_T *command, size_t bytes, uint32_t last);
static size_t command_batch_limit(struct VkCommandBuffer_T *command);
static VkResult command_pending_send(struct VkCommandBuffer_T *command);
static VkResult command_replies(struct vulkan_context *context, struct vulkan_reader *reader, VkResult status, const uint32_t *expected, uint32_t count);
static VkResult command_send_later(struct VkCommandBuffer_T *command, size_t start, size_t length);
static void command_sent_wait(struct VkCommandBuffer_T *command);
static void command_pool_sent_wait(struct vulkan_command_pool *pool);
static void command_sender_start(void);
static void *command_sender(void *argument);
static void command_encode_color(struct vulkan_writer *writer, const VkClearColorValue *color);
static void command_encode_depth_ranges(struct vulkan_writer *writer, const VkClearDepthStencilValue *clear, uint32_t count, const VkImageSubresourceRange *ranges);
static void command_encode_clear_attachment(struct vulkan_writer *writer, const VkClearAttachment *attachment);
static void command_encode_render_begin(struct vulkan_writer *writer, const VkRenderPassBeginInfo *info);
static void command_encode_render_clear(struct vulkan_writer *writer, const VkAttachmentDescription *attachment, const VkClearValue *clear);
static void command_buffers_local_free(uint32_t count, VkCommandBuffer *buffers);
static void command_buffer_initial(struct VkCommandBuffer_T *command);
static void command_buffer_release(struct vulkan_object *object);

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
	vulkan_command_begin(&writer, GPU_OP_CREATE_COMMAND_POOL);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_encode_VkCommandPoolCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, GPU_OP_DESTROY_COMMAND_POOL);
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

	/* The batches its command buffers still send go before the pool. */
	command_pool_sent_wait(pool);

	/* Native pool destruction consumes its native command buffers in the same operation. */
	status = vulkan_object_destroy_remote(owner, &pool->object, GPU_OP_DESTROY_COMMAND_POOL);
	if (status != VK_SUCCESS)
		vulkan_context_error(pool->object.context, VK_ERROR_DEVICE_LOST);

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
	command_pool_sent_wait(pool);
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, GPU_OP_RESET_COMMAND_POOL);
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
		/* Release-resources drops retained recording capacity only after native reset. */
		if (flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT)
			command_buffer_release(child);

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
		command->object.release_storage = command_buffer_release;
		command->recording.allocator = command->object.allocator;
		command->recording.scope = VK_SYSTEM_ALLOCATION_SCOPE_OBJECT;
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
	vulkan_command_begin(&writer, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
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

	/* The batches the command buffers still send go before the free. */
	for (index = 0; index < commandBufferCount; index++) {
		command = (struct VkCommandBuffer_T *)pCommandBuffers[index];
		if (command != NULL)
			command_sent_wait(command);
	}

	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, GPU_OP_FREE_COMMAND_BUFFERS);
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
		vulkan_context_error(owner->object.context, VK_ERROR_DEVICE_LOST);

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
	VkResult status;

	/*
	 * An End still waiting for a submission travels first, so the native
	 * command buffer has left its recording before Begin.  Its result is
	 * not reported: Begin discards that recording, and a lost device is
	 * latched on the context.
	 */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	command_sent_wait(command);
	if (command->end_pending != VK_FALSE)
		(void)command_pending_send(command);

	/* Begin discards the earlier recording's bytes and failure. */
	command->recording.bytes = 0;
	command->recording.error = VK_SUCCESS;

	/* The discarded recording's swapchain-image writes are never submitted. */
	vulkan_content_clear(command);

	/* Primary buffers never dereference their ignored inheritance pointer. */
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

	/*
	 * Native Begin applies the standard initial/re-recording transition
	 * and pool flags.  It goes, after a Reset left for it, at the
	 * recording's start and travels with the recording's first bytes (a
	 * batch or End), which read its reply: a failure only the renderer
	 * sees is reported there.
	 */
	command->head_replies = 0;
	if (command->reset_pending != VK_FALSE) {
		command_head_add(command, GPU_OP_RESET_COMMAND_BUFFER);
		vulkan_write_u64(&command->recording, command->object.wire_id);
		vulkan_write_u32(&command->recording, command->reset_flags);
	}
	command_head_add(command, GPU_OP_BEGIN_COMMAND_BUFFER);
	vulkan_write_u64(&command->recording, command->object.wire_id);
	vulkan_write_u64(&command->recording, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&command->recording, &begin);
	status = command->recording.error;
	if (status != VK_SUCCESS) {
		command->recording.bytes = 0;
		command->recording.error = VK_SUCCESS;
		command->head_replies = 0;
		command->head_bytes = 0;
		command->error = status;
		command->state = VULKAN_COMMAND_INVALID;
		return status;
	}
	command->head_bytes = command->recording.bytes;
	command->reset_pending = VK_FALSE;

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
	size_t limit;

	/* Prepares the sole result-bearing record after every deferred void command. */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	command_sent_wait(command);
	prior = command->error;
	vulkan_writer_init_for_object(&writer, &command->object);
	vulkan_command_begin(&writer, GPU_OP_END_COMMAND_BUFFER);
	vulkan_write_u64(&writer, command->object.wire_id);

	/* Appends only a complete End record to an otherwise valid recording. */
	if (prior == VK_SUCCESS) {
		prior = writer.error;
		if (prior == VK_SUCCESS) {
			vulkan_write_bytes(&command->recording, writer.data, writer.bytes);
			prior = command->recording.error;
		}
	}

	/*
	 * A primary command buffer's recording, End included, waits for the
	 * submission that names it when it is small enough to travel with
	 * that submission: it is executable now, and a failure only the
	 * renderer sees is reported by vkQueueSubmit.
	 */
	if (prior == VK_SUCCESS && command->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
		limit = command_batch_limit(command) / 4U;
		if (command->recording.bytes <= limit) {
			vulkan_writer_finish(&writer);
			command->end_pending = VK_TRUE;
			command->state = VULKAN_COMMAND_EXECUTABLE;

			/* Succeeded: the submission carries the End. */
			return VK_SUCCESS;
		}
	}

	/*
	 * A failed local record still ends the already-started native
	 * recording: End alone, after the head when it has not travelled yet.
	 * Else the recording with End, whose reply comes after the head's.
	 */
	if (prior != VK_SUCCESS) {
		command->recording.bytes = command->head_bytes;
		command->recording.error = VK_SUCCESS;
		if (command->head_bytes == 0) {
			status = vulkan_command_execute(command->object.context, &writer, 8, &reader, VK_TRUE);
			status = vulkan_reply_finish(command->object.context, &reader, status);
		} else {
			vulkan_write_bytes(&command->recording, writer.data, writer.bytes);
			status = command->recording.error;
			if (status == VK_SUCCESS)
				status = command_send(command, command->recording.bytes, GPU_OP_END_COMMAND_BUFFER);
		}
	} else {
		status = command_send(command, command->recording.bytes, GPU_OP_END_COMMAND_BUFFER);
	}

	/* Retains capacity for re-recording, never bytes that may already be native. */
	command->recording.bytes = 0;
	command->head_bytes = 0;
	command->head_replies = 0;
	vulkan_writer_finish(&writer);

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
	VkCommandBufferResetFlags pending;

	/*
	 * The native reset, with the requested resource-release behaviour,
	 * waits for the next Begin and travels at its head (only Begin may
	 * follow a reset command buffer); a failure only the renderer sees is
	 * reported where Begin's is.  Two resets in a row release resources
	 * when either asked to.
	 */
	command = (struct VkCommandBuffer_T *)commandBuffer;
	command_sent_wait(command);
	pending = 0U;
	if (command->reset_pending != VK_FALSE)
		pending = command->reset_flags;

	/* Release-resources returns the local callback storage now. */
	if (flags & VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT)
		command_buffer_release(&command->object);

	/* The local initial state, and the native reset left for Begin. */
	command_buffer_initial(command);
	command->reset_pending = VK_TRUE;
	command->reset_flags = pending | flags;

	/* Succeeded: this command buffer may be recorded again under its pool's normal rules. */
	return VK_SUCCESS;
}

/*
 * Starts one video coding record with the ordinary recording framing.
 */
VkBool32
vulkan_command_record_begin(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer,
	uint32_t opcode)
{
	VkBool32 active;

	/* Applies the same state and earlier-failure checks as every vkCmd* call. */
	active = command_record_begin(command, writer, opcode);
	if (!active)
		return VK_FALSE;

	/* Succeeded: the caller appends the video command's arguments. */
	return VK_TRUE;
}

/*
 * Retains one complete video coding record in its command buffer.
 */
void
vulkan_command_record_finish(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer)
{
	/* Appends or flushes the record exactly as an ordinary command is. */
	command_record_finish(command, writer);

	/* Succeeded: the record is kept or its failure is latched on the command buffer. */
	return;
}

/*
 * Writes a command buffer's recording whose End waits for its submission
 * ahead of the submission attempt serial, once, and counts the replies it
 * asks for.  A recording that would grow the transaction past one batch
 * is sent alone first (its failure is the command buffer's, which the
 * submission then reports).
 */
VkResult
vulkan_command_pending_write(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer,
	uint64_t serial,
	uint32_t *replies)
{
	size_t limit;
	VkResult status;

	/* Only an End still waiting, and only once in this attempt. */
	if (command->end_pending == VK_FALSE)
		return VK_SUCCESS;
	if (command->end_serial == serial)
		return VK_SUCCESS;

	/* A transaction past one batch: this recording travels alone, now. */
	limit = command_batch_limit(command);
	if (writer->bytes + command->recording.bytes > limit) {
		status = command_pending_send(command);
		if (status != VK_SUCCESS) {
			command->error = status;
			command->state = VULKAN_COMMAND_INVALID;
		}

		/* Succeeded: the recording travelled, or its failure waits on the command buffer. */
		return VK_SUCCESS;
	}

	/* The recording, End included, and the replies of its head and End. */
	vulkan_write_bytes(writer, command->recording.data, command->recording.bytes);
	command->end_serial = serial;
	*replies += command->head_replies + 1U;

	/* Succeeded: the recording goes with the submission's transaction. */
	return writer->error;
}

/*
 * Reads the replies of a recording written ahead of the submission
 * attempt serial, which ran: the head's, then End's.  The recording has
 * travelled whatever they say; a failure makes the command buffer
 * invalid and is returned.
 */
VkResult
vulkan_command_pending_read(
	struct VkCommandBuffer_T *command,
	struct vulkan_reader *reader,
	uint64_t serial)
{
	uint32_t expected;
	uint32_t opcode;
	uint32_t index;
	VkResult status;
	VkResult result;

	/* Only a recording this attempt wrote, read once. */
	if (command->end_pending == VK_FALSE)
		return VK_SUCCESS;
	if (command->end_serial != serial)
		return VK_SUCCESS;

	/* Each reply in the order its command lay: its opcode, then its result; the first failure is kept. */
	result = VK_SUCCESS;
	for (index = 0U; index <= command->head_replies; index++) {
		expected = GPU_OP_END_COMMAND_BUFFER;
		if (index < command->head_replies)
			expected = command->head_opcodes[index];
		opcode = vulkan_read_u32(reader);
		if (reader->error != VK_SUCCESS || opcode != expected) {
			reader->error = VK_ERROR_DEVICE_LOST;
			break;
		}
		status = vulkan_read_result(reader);
		if (result == VK_SUCCESS)
			result = status;
		if (reader->error != VK_SUCCESS)
			break;
	}

	/* The recording has travelled. */
	command->end_pending = VK_FALSE;
	command->recording.bytes = 0;
	command->head_bytes = 0;
	command->head_replies = 0;
	if (reader->error != VK_SUCCESS)
		result = reader->error;

	/* A failure the renderer saw makes the command buffer invalid. */
	if (result != VK_SUCCESS) {
		command->error = result;
		command->state = VULKAN_COMMAND_INVALID;
		return result;
	}

	/* Succeeded: the native command buffer ended executable. */
	return VK_SUCCESS;
}

#include "commands-generated.inc"

/*
 * Starts one ordinary recording call without losing an earlier encoding
 * failure.  The record is encoded straight after the recording's bytes:
 * the writer takes the recording's storage over until
 * command_record_finish() gives it back, which every call that started
 * one does.
 */
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

	/* The recording's storage, its policy and the renderer's native memory type, the record from its end. */
	*writer = command->recording;
	writer->opcode = opcode;
	writer->error = VK_SUCCESS;
	if (command->object.context != NULL)
		writer->external_memory_type = command->object.context->external_memory_type;
	command->record_start = writer->bytes;
	vulkan_record_u32(writer, opcode);
	vulkan_record_u32(writer, 0);
	vulkan_record_u64(writer, command->object.wire_id);

	/* Succeeded: typed argument encoders may append to this recording operation. */
	return VK_TRUE;
}

/*
 * Keeps a complete no-reply record and bounds storage at record
 * boundaries: the recording's storage comes back from the writer, a
 * record that did not encode is cut off, the prefix before a record that
 * would cross the soft limit is decoded first, and a single record over
 * the limit is decoded alone.
 */
static void
command_record_finish(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer)
{
	struct vulkan_reader reader;
	size_t limit;
	size_t start;
	size_t length;
	VkResult status;

	/* The storage, possibly grown, is the recording's again; the record is what follows its start. */
	status = writer->error;
	start = command->record_start;
	command->recording = *writer;
	command->recording.error = VK_SUCCESS;
	memset(writer, 0, sizeof(*writer));
	length = command->recording.bytes - start;
	if (status != VK_SUCCESS)
		command->recording.bytes = start;

	/* Leave space for the final result-bearing End record. */
	limit = command_batch_limit(command);

	/* An unusably small backend cannot retain one complete record and End. */
	if (status == VK_SUCCESS) {
		if (limit < 16) {
			status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			command->recording.bytes = start;
		} else {
			limit -= 16;
		}
	}

	/* The complete prefix before this record goes (to the recording thread) when the two would cross the soft limit. */
	if (status == VK_SUCCESS && start != 0 && command->recording.bytes > limit)
		status = command_send_later(command, start, length);

	/* A single record over the limit is decoded alone through the existing external-stream transport, after the batches before it. */
	if (status == VK_SUCCESS && length > limit) {
		command_sent_wait(command);
		status = command->error;
	}
	if (status == VK_SUCCESS && length > limit) {
		status = vulkan_context_execute(command->object.context, &command->recording, 0, &reader);
		status = vulkan_reply_finish(command->object.context, &reader, status);
		command->recording.bytes = 0;
	}

	/* A failure is latched on the command buffer. */
	if (status != VK_SUCCESS) {
		command->error = status;
		command->state = VULKAN_COMMAND_INVALID;
		return;
	}

	/* Succeeded: recording is owned locally or its complete prefix was decoded. */
	return;
}

/*
 * Puts one more reply-asking command at the recording's head (its header
 * now; the caller writes its body).
 */
static void
command_head_add(
	struct VkCommandBuffer_T *command,
	uint32_t opcode)
{
	/* The header asks for a reply, which the head's sending reads. */
	vulkan_write_u32(&command->recording, opcode);
	vulkan_write_u32(&command->recording, GPU_OP_FLAG_REPLY);
	command->head_opcodes[command->head_replies] = opcode;
	command->head_replies++;

	/* Succeeded: the command is at the head. */
	return;
}

/*
 * Sends the recording's first bytes in one transaction and reads the
 * replies they ask for: the head's, while it has not travelled, then
 * last's (an opcode, or 0 for none).  Returns the first failure.
 */
static VkResult
command_send(
	struct VkCommandBuffer_T *command,
	size_t bytes,
	uint32_t last)
{
	struct vulkan_context *context;
	struct vulkan_writer part;
	struct vulkan_reader reader;
	uint32_t expected[3];
	uint32_t count;
	uint32_t index;
	VkResult status;
	VkResult result;

	/* The replies in the order their commands lie. */
	context = command->object.context;
	count = 0U;
	for (index = 0U; index < command->head_replies; index++)
		expected[count++] = command->head_opcodes[index];
	if (last != 0U)
		expected[count++] = last;

	/* The bytes, timed under the first command that answers. */
	part = command->recording;
	part.bytes = bytes;
	part.error = VK_SUCCESS;
	if (count != 0U)
		part.opcode = expected[0];
	status = vulkan_context_execute(context, &part, (size_t)count * 8U, &reader);

	/* The head has travelled whatever the result. */
	command->head_replies = 0U;
	command->head_bytes = 0U;

	/* The replies, in order. */
	result = command_replies(context, &reader, status, expected, count);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the bytes ran and every answering command succeeded. */
	return VK_SUCCESS;
}

/*
 * Reads the replies of a transaction that ran with status: each its own
 * opcode (expected, count of them), then its result.  Returns the
 * transport's failure, else the first answering command's failure.
 */
static VkResult
command_replies(
	struct vulkan_context *context,
	struct vulkan_reader *reader,
	VkResult status,
	const uint32_t *expected,
	uint32_t count)
{
	uint32_t index;
	uint32_t opcode;
	VkResult result;

	/* Each reply: its own opcode, then its result; the first failure is kept. */
	result = VK_SUCCESS;
	for (index = 0U; status == VK_SUCCESS && index < count; index++) {
		opcode = vulkan_read_u32(reader);
		if (reader->error != VK_SUCCESS || opcode != expected[index]) {
			reader->error = VK_ERROR_DEVICE_LOST;
			vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
			break;
		}
		status = vulkan_read_result(reader);
		if (status == VK_ERROR_DEVICE_LOST)
			vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		if (result == VK_SUCCESS)
			result = status;
		status = VK_SUCCESS;
		if (reader->error != VK_SUCCESS)
			break;
	}
	status = vulkan_reply_finish(context, reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Reports the first answering command's failure, or success. */
	return result;
}

/*
 * Hands the recording's complete prefix (start bytes, the head's replies
 * with it) to the recording thread and keeps recording into a place of
 * its own, the record after the prefix (length bytes) moved there first.
 * Without the thread the prefix is sent now, as before.  Returns the
 * failure of a prefix sent now, or of the move.
 */
static VkResult
command_send_later(
	struct VkCommandBuffer_T *command,
	size_t start,
	size_t length)
{
	struct vulkan_recording_batch *batch;
	struct vulkan_writer spare;
	uint32_t index;
	VkResult status;
	int locked;

	/* Starts the recording thread once; without it the prefix is sent here. */
	(void)pthread_once(&command_sender_once, command_sender_start);
	if (!command_sender_started) {
		status = command_send(command, start, 0U);
		if (status != VK_SUCCESS) {
			command->recording.bytes = 0;
			return status;
		}
		memmove(command->recording.data, command->recording.data + start, length);
		command->recording.bytes = length;

		/* Succeeded: the prefix ran and the record moved to the front. */
		return VK_SUCCESS;
	}

	/* The next place, once the thread has sent what it held. */
	batch = &command->batches[command->batch_next];
	locked = pthread_mutex_lock(&command_sender_lock);
	if (locked != 0)
		abort();

	/* Waits while the thread still sends the batch this place held. */
	while (batch->waiting != VK_FALSE)
		(void)pthread_cond_wait(&command_sender_done, &command_sender_lock);

	(void)pthread_mutex_unlock(&command_sender_lock);

	/*
	 * The prefix goes to the place, which gives its storage (kept from an
	 * earlier batch, or none yet) to the recording; the record is put at
	 * the recording's start.
	 */
	spare = batch->part;
	batch->part = command->recording;
	batch->part.bytes = start;
	batch->part.error = VK_SUCCESS;
	command->recording = spare;
	command->recording.allocator = command->object.allocator;
	command->recording.scope = VK_SYSTEM_ALLOCATION_SCOPE_OBJECT;
	command->recording.opcode = batch->part.opcode;
	command->recording.external_memory_type = batch->part.external_memory_type;
	command->recording.bytes = 0;
	command->recording.error = VK_SUCCESS;
	vulkan_write_bytes(&command->recording, batch->part.data + start, length);
	status = command->recording.error;
	if (status != VK_SUCCESS) {
		command->recording.bytes = 0;
		command->recording.error = VK_SUCCESS;
		return status;
	}

	/* The replies the head asks for travel with the prefix; the head has travelled. */
	batch->command = command;
	batch->count = command->head_replies;
	for (index = 0U; index < command->head_replies; index++)
		batch->expected[index] = command->head_opcodes[index];
	command->head_replies = 0U;
	command->head_bytes = 0U;

	/* Queued last: the thread sends batches in the order they were handed over. */
	locked = pthread_mutex_lock(&command_sender_lock);
	if (locked != 0)
		abort();

	batch->waiting = VK_TRUE;
	batch->next = NULL;
	if (command_sender_last != NULL)
		command_sender_last->next = batch;
	else
		command_sender_first = batch;
	command_sender_last = batch;
	command->sent_pending++;
	(void)pthread_cond_signal(&command_sender_ready);

	(void)pthread_mutex_unlock(&command_sender_lock);

	command->batch_next = (command->batch_next + 1U) % VULKAN_RECORDING_BATCHES;

	/* Succeeded: the prefix is the thread's to send. */
	return VK_SUCCESS;
}

/*
 * Waits until the recording thread has sent every batch of a command
 * buffer, and makes the first failure they met the command buffer's.
 */
static void
command_sent_wait(
	struct VkCommandBuffer_T *command)
{
	VkResult failure;
	int locked;

	/* Nothing was ever handed over. */
	if (!command_sender_started)
		return;

	/* The batches sent, their failure taken. */
	locked = pthread_mutex_lock(&command_sender_lock);
	if (locked != 0)
		abort();

	/* Waits while any batch of the command buffer is still to be sent. */
	while (command->sent_pending != 0U)
		(void)pthread_cond_wait(&command_sender_done, &command_sender_lock);
	failure = command->sent_error;
	command->sent_error = VK_SUCCESS;

	(void)pthread_mutex_unlock(&command_sender_lock);

	/* A batch the renderer refused makes the recording invalid. */
	if (failure != VK_SUCCESS && command->error == VK_SUCCESS) {
		command->error = failure;
		command->state = VULKAN_COMMAND_INVALID;
	}
}

/* Waits until the recording thread has sent the batches of every command buffer of a pool. */
static void
command_pool_sent_wait(
	struct vulkan_command_pool *pool)
{
	struct vulkan_object *child;

	/* Each command buffer of the pool (the application keeps the pool to this thread). */
	for (child = pool->object.first_child; child != NULL; child = child->next_sibling)
		command_sent_wait((struct VkCommandBuffer_T *)child);
}

/* Starts the recording thread (once); a thread that cannot start leaves batches to the recording calls. */
static void
command_sender_start(
	void)
{
	pthread_attr_t attributes;
	pthread_t thread;
	int error;

	/* A detached thread for the life of the process. */
	error = pthread_attr_init(&attributes);
	if (error != 0)
		return;
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&thread, &attributes, command_sender, NULL);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0)
		return;

	/* The thread takes batches from now on. */
	command_sender_started = VK_TRUE;
}

/*
 * Sends the batches handed over, one at a time in their order, each in
 * one transaction with the replies its head asks for, for the life of
 * the process.
 */
static void *
command_sender(
	void *argument)
{
	struct vulkan_recording_batch *batch;
	struct VkCommandBuffer_T *command;
	struct vulkan_context *context;
	struct vulkan_reader reader;
	VkResult status;
	int locked;

	(void)argument;

	/* One batch after another, waiting when there is none. */
	for (;;) {
		/* Takes the first batch waiting. */
		locked = pthread_mutex_lock(&command_sender_lock);
		if (locked != 0)
			abort();

		/* Waits while no batch is handed over. */
		while (command_sender_first == NULL)
			(void)pthread_cond_wait(&command_sender_ready, &command_sender_lock);
		batch = command_sender_first;
		command_sender_first = batch->next;
		if (command_sender_first == NULL)
			command_sender_last = NULL;

		(void)pthread_mutex_unlock(&command_sender_lock);

		/* The batch's transaction and its head's replies. */
		command = batch->command;
		context = command->object.context;
		if (batch->count != 0U)
			batch->part.opcode = batch->expected[0];
		status = vulkan_context_execute(context, &batch->part, (size_t)batch->count * 8U, &reader);
		status = command_replies(context, &reader, status, batch->expected, batch->count);

		/* Sent: its failure is the command buffer's, its place free again. */
		locked = pthread_mutex_lock(&command_sender_lock);
		if (locked != 0)
			abort();

		if (status != VK_SUCCESS && command->sent_error == VK_SUCCESS)
			command->sent_error = status;
		command->sent_pending--;
		batch->waiting = VK_FALSE;
		(void)pthread_cond_broadcast(&command_sender_done);

		(void)pthread_mutex_unlock(&command_sender_lock);
	}

	/* Not reached: the thread lives as long as the process. */
	return NULL;
}

/* The bytes one transaction of a command buffer's recording may carry. */
static size_t
command_batch_limit(
	struct VkCommandBuffer_T *command)
{
	size_t limit;

	/* One batch, or less when the backend's resources are smaller. */
	limit = VULKAN_RECORDING_BATCH_BYTES;
	if (command->object.context->max_resource_bytes != 0) {
		if (command->object.context->max_resource_bytes < limit)
			limit = (size_t)command->object.context->max_resource_bytes;
	}

	/* Succeeded: the limit is known. */
	return limit;
}

/*
 * Sends a recording whose End waits for a submission in its own
 * transaction, with the head's and End's replies; it has travelled
 * whatever the result.  Returns the first failure.
 */
static VkResult
command_pending_send(
	struct VkCommandBuffer_T *command)
{
	VkResult status;

	/* The recording, End included, and the replies it asks for. */
	status = command_send(command, command->recording.bytes, GPU_OP_END_COMMAND_BUFFER);

	/* The recording has travelled. */
	command->end_pending = VK_FALSE;
	command->recording.bytes = 0;
	command->head_bytes = 0;
	command->head_replies = 0;
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the native command buffer ended executable. */
	return VK_SUCCESS;
}

/* Releases retained bytes through the command's effective destruction policy. */
static void
command_buffer_release(
	struct vulkan_object *object)
{
	struct VkCommandBuffer_T *command;
	uint32_t index;

	/* The batches still being sent first; parent destruction may supply compatible callbacks with new user data. */
	command = (struct VkCommandBuffer_T *)object;
	command_sent_wait(command);

	/* The storage each batch's place kept. */
	for (index = 0U; index < VULKAN_RECORDING_BATCHES; index++) {
		command->batches[index].part.allocator = object->allocator;
		vulkan_writer_finish(&command->batches[index].part);
	}

	/* The recording's own storage. */
	command->recording.allocator = object->allocator;
	vulkan_writer_finish(&command->recording);
	command->recording.allocator = object->allocator;
	command->recording.scope = VK_SYSTEM_ALLOCATION_SCOPE_OBJECT;

	/* Succeeded: no separately allocated recording storage remains owned. */
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
	/* Batches still being sent belong to the discarded recording; they are sent first. */
	command_sent_wait(command);

	/* Recording errors belong to the discarded old recording and cannot survive a successful reset. */
	command->recording.bytes = 0;
	command->recording.error = VK_SUCCESS;
	command->head_bytes = 0;
	command->head_replies = 0;
	command->reset_pending = VK_FALSE;
	command->end_pending = VK_FALSE;
	command->error = VK_SUCCESS;
	command->state = VULKAN_COMMAND_INITIAL;

	/* The discarded recording's swapchain-image writes are never submitted. */
	vulkan_content_clear(command);

	/* Succeeded: this command buffer is ready for a fresh standard Begin operation. */
	return;
}
