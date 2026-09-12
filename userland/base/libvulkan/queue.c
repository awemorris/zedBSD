/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Submits native work while consuming completed acquisition payloads exactly once.
 */

#include <stdlib.h>
#include <string.h>
#include "sync-internal.h"

/* One wait records whether this transaction will consume a software completion. */
struct vulkan_queue_wait {
	struct vulkan_sync *sync;
	VkBool32 software;
};

/* One enqueue owns its proposed wait transitions until native acceptance. */
struct vulkan_queue_transaction {
	struct vulkan_queue_wait *waits;
	size_t count;
};

static VkResult queue_enqueue(struct VkQueue_T *queue, uint32_t count, const VkSubmitInfo *submits, const VkBindSparseInfo *binds, VkFence fence, VkBool32 sparse);
static void queue_write_waits(struct vulkan_writer *writer, struct vulkan_queue_transaction *transaction, uint32_t count, const VkSemaphore *semaphores, const VkPipelineStageFlags *stages, VkBool32 has_stages);
static void queue_write_signals(struct vulkan_writer *writer, uint32_t count, const VkSemaphore *semaphores);
static void queue_write_submit(struct vulkan_writer *writer, struct vulkan_queue_transaction *transaction, const VkSubmitInfo *submit);
static void queue_write_sparse(struct vulkan_writer *writer, struct vulkan_queue_transaction *transaction, const VkBindSparseInfo *bind);
static void queue_write_memory_bind(struct vulkan_writer *writer, const VkSparseMemoryBind *bind);
static void queue_write_image_bind(struct vulkan_writer *writer, const VkSparseImageMemoryBind *bind);
static void queue_write_handle(struct vulkan_writer *writer, uint64_t handle);

/*
 * Submits ordinary Vulkan command buffers and binary synchronization operations.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(
	VkQueue queue,
	uint32_t submitCount,
	const VkSubmitInfo *pSubmits,
	VkFence fence)
{
	struct VkQueue_T *owner;
	VkResult status;

	/* Public and WSI submitters share the same ownership and payload transaction. */
	owner = vulkan_queue(queue);
	status = vulkan_queue_submit(owner, submitCount, pSubmits, fence);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the native queue accepted the complete requested submission. */
	return VK_SUCCESS;
}

/*
 * Enqueues sparse bindings with the same binary wait semantics as command submission.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueBindSparse(
	VkQueue queue,
	uint32_t bindInfoCount,
	const VkBindSparseInfo *pBindInfo,
	VkFence fence)
{
	struct VkQueue_T *owner;
	VkResult status;

	/* Encode valid sparse calls without inventing a device-specific fallback result. */
	owner = vulkan_queue(queue);
	status = queue_enqueue(owner, bindInfoCount, NULL, pBindInfo, fence, VK_TRUE);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the sparse binding and synchronization operations were accepted. */
	return VK_SUCCESS;
}

/*
 * Waits for presentation and GPU work belonging to one ordinary queue.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle(
	VkQueue queue)
{
	struct VkQueue_T *owner;
	VkResult status;

	/* Drain presentation producers before placing the final native queue marker. */
	owner = vulkan_queue(queue);
	status = vulkan_wsi_queue_idle(owner);
	if (status != VK_SUCCESS)
		return status;

	/* The internal fence wait owns no queue mutex while GPU execution is pending. */
	status = vulkan_queue_idle(owner);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: this queue has no earlier GPU or presentation work outstanding. */
	return VK_SUCCESS;
}

/*
 * Waits for every requested queue and native presentation owned by a device.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(
	VkDevice device)
{
	struct VkDevice_T *owner;
	VkResult status;
	uint32_t index;

	/* Native presentation producers must stop submitting before queue markers. */
	owner = vulkan_device(device);
	status = vulkan_wsi_device_idle(owner);
	if (status != VK_SUCCESS)
		return status;

	/* Application external synchronization keeps the created queue array stable. */
	for (index = 0; index < owner->queue_count; index++) {
		status = vulkan_queue_idle(owner->queues[index]);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Succeeded: all logical-device GPU and presentation work has completed. */
	return VK_SUCCESS;
}

/*
 * Serializes an internal or public enqueue without retaining locks for GPU completion.
 */
VkResult
vulkan_queue_submit(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	VkFence fence)
{
	VkResult status;

	/* Commit native acceptance and completed acquisition waits together. */
	status = queue_enqueue(queue, count, submits, NULL, fence, VK_FALSE);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: ownership has passed to the actual GPU queue. */
	return VK_SUCCESS;
}

/*
 * Waits for a private empty-submit fence without using unsupported idle opcodes.
 */
VkResult
vulkan_queue_idle(
	struct VkQueue_T *queue)
{
	VkFenceCreateInfo create;
	VkFence fence;
	VkResult status;

	/* A private fence gives this operation its own completion lifetime. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence((VkDevice)queue->device, &create, NULL, &fence);
	if (status != VK_SUCCESS)
		return status;

	/* An empty submit fence covers all earlier work on the same native queue. */
	status = vulkan_queue_submit(queue, 0, NULL, fence);
	if (status != VK_SUCCESS) {
		vkDestroyFence((VkDevice)queue->device, fence, NULL);
		return status;
	}

	/* Infinite API waiting still permits another thread to unblock a host event. */
	status = vulkan_fences_wait(queue->device, 1, &fence, VK_TRUE, UINT64_MAX);
	if (status != VK_SUCCESS) {
		vkDestroyFence((VkDevice)queue->device, fence, NULL);
		return status;
	}

	/* Reclaim only the marker whose GPU use has now completed. */
	vkDestroyFence((VkDevice)queue->device, fence, NULL);

	/* Succeeded: every earlier native queue operation has completed. */
	return VK_SUCCESS;
}

/* Commit proposed software waits only after one complete native enqueue succeeds. */
static VkResult
queue_enqueue(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	const VkBindSparseInfo *binds,
	VkFence fence,
	VkBool32 sparse)
{
	struct VkDevice_T *device;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	struct vulkan_queue_transaction transaction;
	struct vulkan_sync *completion;
	VkResult status;
	size_t capacity;
	size_t index;
	uint32_t request;
	uint32_t waits;

	/* Count caller waits before locks or wire output so allocation cannot consume state. */
	device = queue->device;
	capacity = 0;
	for (request = 0; request < count; request++) {
		/* Sparse binding and command submission carry identical binary wait lists. */
		if (sparse) {
			waits = binds[request].waitSemaphoreCount;
		} else {
			waits = submits[request].waitSemaphoreCount;
		}

		/* Bound aggregate wait storage on either standard pointer-width ABI. */
		if (waits > SIZE_MAX - capacity)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Count every occurrence so proposed consumption can follow submission order. */
		capacity += waits;
	}

	/* A transaction with no waits needs no temporary allocation. */
	memset(&transaction, 0, sizeof(transaction));
	if (capacity != 0) {
		/* Check multiplication before allocating the complete rollback ledger. */
		if (sizeof(*transaction.waits) > SIZE_MAX / capacity)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Keep proposed transitions private until native command acceptance. */
		transaction.waits = vulkan_allocate(&device->object.allocator, capacity * sizeof(*transaction.waits), sizeof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		if (transaction.waits == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Begin with no proposed consumption in any newly allocated wait slot. */
		memset(transaction.waits, 0, capacity * sizeof(*transaction.waits));
	}

	/* Resolve the optional fence before entering the queue-to-device lock order. */
	completion = vulkan_sync_object((uint64_t)(uintptr_t)fence);
	vulkan_writer_init_for_object(&writer, &queue->object);
	pthread_mutex_lock(&queue->mutex);

	/* Acquire briefly shared binary-payload state only while enqueuing native work. */
	pthread_mutex_lock(&device->mutex);

	/* Both native commands begin with queue identity and the counted record array. */
	if (sparse) {
		vulkan_command_begin(&writer, VULKAN_OPCODE_vkQueueBindSparse);
	} else {
		vulkan_command_begin(&writer, VULKAN_OPCODE_vkQueueSubmit);
	}

	/* Encoding failure prevents any host side effect or software-payload commit. */
	status = vulkan_sync_device_status_locked(device);
	if (status != VK_SUCCESS)
		writer.error = status;

	/* Append only after command framing retained its own allocation status. */
	vulkan_write_u64(&writer, queue->object.wire_id);
	vulkan_write_u32(&writer, count);
	vulkan_write_u64(&writer, count);
	for (request = 0; request < count; request++) {
		/* Preserve the caller's record ordering across all waits and signals. */
		if (sparse) {
			queue_write_sparse(&writer, &transaction, &binds[request]);
		} else {
			queue_write_submit(&writer, &transaction, &submits[request]);
		}
	}

	/* Native fence completion represents all successfully enqueued records. */
	queue_write_handle(&writer, (uint64_t)(uintptr_t)fence);
	status = vulkan_command_execute(device->object.context, &writer, 8, &reader, VK_TRUE);
	if (status == VK_SUCCESS) {
		/* Software acquisition payloads disappear only after the matching wait is accepted. */
		for (index = 0; index < transaction.count; index++) {
			/* Native waits keep their host payload unchanged until GPU execution. */
			if (transaction.waits[index].software)
				transaction.waits[index].sync->software_signaled = VK_FALSE;
		}

		/* A submitted fence now obtains completion from its native queue payload. */
		if (completion != NULL)
			completion->software_signaled = VK_FALSE;
	}

	/* Record device loss before releasing locally observable sync state. */
	vulkan_sync_device_error(device, status);

	pthread_mutex_unlock(&device->mutex);

	pthread_mutex_unlock(&queue->mutex);

	/* Failed encoding or enqueue leaves every proposed software wait unconsumed. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	vulkan_free(&device->object.allocator, transaction.waits);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: native enqueue and local payload consumption are one transaction. */
	return VK_SUCCESS;
}

/* Elide only the first wait that consumes each already completed acquisition. */
static void
queue_write_waits(
	struct vulkan_writer *writer,
	struct vulkan_queue_transaction *transaction,
	uint32_t count,
	const VkSemaphore *semaphores,
	const VkPipelineStageFlags *stages,
	VkBool32 has_stages)
{
	struct vulkan_sync *sync;
	size_t first;
	size_t previous;
	size_t position;
	uint32_t index;
	uint32_t native_count;
	VkBool32 software;

	/* Classify the whole list before encoding its reduced native cardinality. */
	first = transaction->count;
	native_count = 0;
	for (index = 0; index < count; index++) {
		/* The device mutex stabilizes all active binary payloads during this scan. */
		sync = vulkan_sync_object((uint64_t)(uintptr_t)semaphores[index]);
		software = sync->software_signaled;
		for (previous = 0; previous < transaction->count; previous++) {
			/* A later wait observes any intervening native signal instead of reusing acquisition. */
			if (transaction->waits[previous].sync == sync) {
				/* Only the one earlier software consumption changes this payload source. */
				if (transaction->waits[previous].software)
					software = VK_FALSE;
			}
		}

		/* Retain the proposed transition without changing the live sync object. */
		position = transaction->count;
		transaction->waits[position].sync = sync;
		transaction->waits[position].software = software;
		transaction->count++;
		if (!software)
			native_count++;
	}

	/* The native semaphore array contains only waits still requiring GPU execution. */
	vulkan_write_u32(writer, native_count);
	vulkan_write_u64(writer, native_count);
	for (index = 0; index < count; index++) {
		/* Already completed acquisitions have no pending native signal to wait on. */
		position = first + index;
		if (transaction->waits[position].software)
			continue;

		/* Preserve the native identity of every remaining semaphore. */
		sync = transaction->waits[position].sync;
		vulkan_write_u64(writer, sync->object.wire_id);
	}

	/* Sparse binds have no stage-mask array; command submissions retain matching masks. */
	if (has_stages) {
		vulkan_write_u64(writer, native_count);

		/* Remove stage masks at exactly the same positions as their completed waits. */
		for (index = 0; index < count; index++) {
			/* The shortened arrays must retain one-to-one semaphore/stage correspondence. */
			position = first + index;
			if (transaction->waits[position].software)
				continue;

			/* Retain the caller's full pipeline-stage dependency for a native wait. */
			vulkan_write_u32(writer, stages[index]);
		}
	}

	/* Succeeded: the writer and ledger describe the same ordered wait operation. */
	return;
}

/* Preserve all native signal operations after any completed acquisition waits. */
static void
queue_write_signals(
	struct vulkan_writer *writer,
	uint32_t count,
	const VkSemaphore *semaphores)
{
	uint32_t index;

	/* Native signals remain GPU work even when an earlier payload was software-complete. */
	vulkan_write_u32(writer, count);
	vulkan_write_u64(writer, count);
	for (index = 0; index < count; index++) {
		queue_write_handle(writer, (uint64_t)(uintptr_t)semaphores[index]);
	}

	/* Succeeded: every requested signal keeps its original ordering and identity. */
	return;
}

/* Encode one standard1.0 submit record using resolved object identities. */
static void
queue_write_submit(
	struct vulkan_writer *writer,
	struct vulkan_queue_transaction *transaction,
	const VkSubmitInfo *submit)
{
	struct VkCommandBuffer_T *command;
	uint32_t index;

	/* No selected Vulkan1.0 extension adds a submit pNext structure. */
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_SUBMIT_INFO);
	vulkan_write_u64(writer, 0);
	queue_write_waits(writer, transaction, submit->waitSemaphoreCount, submit->pWaitSemaphores, submit->pWaitDstStageMask, VK_TRUE);

	/* Preserve the ordered command-buffer list and any earlier recording failure. */
	vulkan_write_u32(writer, submit->commandBufferCount);
	vulkan_write_u64(writer, submit->commandBufferCount);
	for (index = 0; index < submit->commandBufferCount; index++) {
		/* The standard dispatchable handle owns its independent renderer identity. */
		command = (struct VkCommandBuffer_T *)submit->pCommandBuffers[index];
		if (command->error != VK_SUCCESS) {
			writer->error = command->error;
			return;
		}

		/* Do not forward application pointer bits across the Venus protocol. */
		vulkan_write_u64(writer, command->object.wire_id);
	}

	/* Native signals follow the command buffers in the same submit record. */
	queue_write_signals(writer, submit->signalSemaphoreCount, submit->pSignalSemaphores);

	/* Succeeded: the record keeps its standard waits, work and signals. */
	return;
}

/* Encode all three sparse binding families without omitting memory-null unbinds. */
static void
queue_write_sparse(
	struct vulkan_writer *writer,
	struct vulkan_queue_transaction *transaction,
	const VkBindSparseInfo *bind)
{
	uint32_t index;
	uint32_t item;
	const VkSparseBufferMemoryBindInfo *buffer;
	const VkSparseImageOpaqueMemoryBindInfo *opaque;
	const VkSparseImageMemoryBindInfo *image;

	/* Sparse binding uses the same completed binary wait transaction. */
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_BIND_SPARSE_INFO);
	vulkan_write_u64(writer, 0);
	queue_write_waits(writer, transaction, bind->waitSemaphoreCount, bind->pWaitSemaphores, NULL, VK_FALSE);

	/* Preserve every buffer binding and its ordered memory ranges. */
	vulkan_write_u32(writer, bind->bufferBindCount);
	vulkan_write_u64(writer, bind->bufferBindCount);
	for (index = 0; index < bind->bufferBindCount; index++) {
		buffer = &bind->pBufferBinds[index];
		queue_write_handle(writer, (uint64_t)(uintptr_t)buffer->buffer);
		vulkan_write_u32(writer, buffer->bindCount);
		vulkan_write_u64(writer, buffer->bindCount);

		/* Retain byte offsets, extents, memory identities and sparse flags. */
		for (item = 0; item < buffer->bindCount; item++) {
			queue_write_memory_bind(writer, &buffer->pBinds[item]);
		}
	}

	/* Opaque image bindings use byte ranges with the same memory-bind representation. */
	vulkan_write_u32(writer, bind->imageOpaqueBindCount);
	vulkan_write_u64(writer, bind->imageOpaqueBindCount);
	for (index = 0; index < bind->imageOpaqueBindCount; index++) {
		opaque = &bind->pImageOpaqueBinds[index];
		queue_write_handle(writer, (uint64_t)(uintptr_t)opaque->image);
		vulkan_write_u32(writer, opaque->bindCount);
		vulkan_write_u64(writer, opaque->bindCount);

		/* Keep opaque ranges distinct from tiled image subresource coordinates. */
		for (item = 0; item < opaque->bindCount; item++) {
			queue_write_memory_bind(writer, &opaque->pBinds[item]);
		}
	}

	/* Tiled image bindings preserve each subresource, spatial region and memory offset. */
	vulkan_write_u32(writer, bind->imageBindCount);
	vulkan_write_u64(writer, bind->imageBindCount);
	for (index = 0; index < bind->imageBindCount; index++) {
		image = &bind->pImageBinds[index];
		queue_write_handle(writer, (uint64_t)(uintptr_t)image->image);
		vulkan_write_u32(writer, image->bindCount);
		vulkan_write_u64(writer, image->bindCount);

		/* Encode every spatial binding independently of host C structure padding. */
		for (item = 0; item < image->bindCount; item++) {
			queue_write_image_bind(writer, &image->pBinds[item]);
		}
	}

	/* Sparse completion signals retain their original native queue semantics. */
	queue_write_signals(writer, bind->signalSemaphoreCount, bind->pSignalSemaphores);

	/* Succeeded: every requested sparse range is represented in caller order. */
	return;
}

/* Encode one opaque sparse memory interval, including an explicit null-memory unbind. */
static void
queue_write_memory_bind(
	struct vulkan_writer *writer,
	const VkSparseMemoryBind *bind)
{
	/* The protocol uses byte-addressed 64-bit offsets on every guest ABI. */
	vulkan_write_u64(writer, bind->resourceOffset);
	vulkan_write_u64(writer, bind->size);
	queue_write_handle(writer, (uint64_t)(uintptr_t)bind->memory);
	vulkan_write_u64(writer, bind->memoryOffset);
	vulkan_write_u32(writer, bind->flags);

	/* Succeeded: no native structure padding or application handle crosses the wire. */
	return;
}

/* Encode one tiled sparse image region and its optional backing allocation. */
static void
queue_write_image_bind(
	struct vulkan_writer *writer,
	const VkSparseImageMemoryBind *bind)
{
	/* Preserve subresource identity independently of the spatial region. */
	vulkan_write_u32(writer, bind->subresource.aspectMask);
	vulkan_write_u32(writer, bind->subresource.mipLevel);
	vulkan_write_u32(writer, bind->subresource.arrayLayer);

	/* Signed offsets retain their exact two's-complement wire representation. */
	vulkan_write_u32(writer, (uint32_t)bind->offset.x);
	vulkan_write_u32(writer, (uint32_t)bind->offset.y);
	vulkan_write_u32(writer, (uint32_t)bind->offset.z);
	vulkan_write_u32(writer, bind->extent.width);
	vulkan_write_u32(writer, bind->extent.height);
	vulkan_write_u32(writer, bind->extent.depth);

	/* A null memory identity explicitly unbinds this image region. */
	queue_write_handle(writer, (uint64_t)(uintptr_t)bind->memory);
	vulkan_write_u64(writer, bind->memoryOffset);
	vulkan_write_u32(writer, bind->flags);

	/* Succeeded: the sparse image record preserves every standard field. */
	return;
}

/* Resolve one nullable non-dispatchable object before writing its native identity. */
static void
queue_write_handle(
	struct vulkan_writer *writer,
	uint64_t handle)
{
	struct vulkan_object *object;
	uint64_t wire_id;

	/* Only the common handle boundary knows the local representation. */
	object = vulkan_nondispatchable_object(handle);
	wire_id = vulkan_object_wire_id(object);
	vulkan_write_u64(writer, wire_id);

	/* Succeeded: null remains zero and ordinary handles use reserved wire identities. */
	return;
}
