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
#include <uapi/gpu-job.h>
#include "sync-internal.h"

#define QUEUE_FENCE_RESET_BATCH 64U

/*
 * A reset is a native transaction: one that would reset fewer than
 * QUEUE_FENCE_RESET_MIN fences makes a new fence instead, until the queue
 * caches QUEUE_FENCE_CACHE_MAX of them, so a queue that submits without
 * fences again and again resets its fences in large batches.
 */
#define QUEUE_FENCE_RESET_MIN 32U
#define QUEUE_FENCE_CACHE_MAX 256U

/* Private identities remain exclusive during unlocked admission waits and GPU execution. */
enum queue_fence_state {
	QUEUE_FENCE_EMPTY,
	QUEUE_FENCE_READY,
	QUEUE_FENCE_PREPARING,
	QUEUE_FENCE_INFLIGHT,
	QUEUE_FENCE_TERMINAL
};

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

/* A queue retains one native fence per concurrent fence-less submission until device teardown. */
struct vulkan_queue_fence {
	struct vulkan_queue_fence *next;
	VkFence fence;
	enum queue_fence_state state;
};

/*
 * The submission attempts, one serial each: a command buffer whose End
 * waits for its submission is written once in an attempt that names it
 * more than once.  Advanced atomically.
 */
static uint64_t queue_end_serial;

static void queue_write_pending(struct vulkan_writer *writer, uint32_t count, const VkSubmitInfo *submits, uint64_t serial, uint32_t *replies);
static VkResult queue_execute_pending(struct VkDevice_T *device, const struct vulkan_writer *writer, uint32_t count, const VkSubmitInfo *submits, uint64_t serial, uint32_t replies, struct vulkan_reader *reader, VkResult *ended);
static VkResult queue_private_fence(struct VkQueue_T *queue, struct vulkan_queue_fence **result);
static VkResult queue_enqueue(struct VkQueue_T *queue, uint32_t count, const VkSubmitInfo *submits, const VkBindSparseInfo *binds, VkFence fence, VkBool32 sparse, VkBool32 locked);
static VkResult queue_attempt(struct VkQueue_T *queue, uint32_t count, const VkSubmitInfo *submits, const VkBindSparseInfo *binds, VkFence fence, VkBool32 sparse, struct vulkan_queue_transaction *transaction, struct vulkan_sync *completion, VkBool32 *accepted, uint64_t *capacity_sequence, VkBool32 *prepared);
static VkResult queue_reset_completed(struct VkQueue_T *queue);
static VkBool32 queue_has_commands(uint32_t count, const VkSubmitInfo *submits);
static VkResult queue_submit_job(struct VkQueue_T *queue, struct vulkan_writer *writer, uint32_t count, const VkSubmitInfo *submits, uint64_t serial, uint32_t replies, struct vulkan_sync *completion, struct vulkan_reader *reader, VkResult *ended, VkBool32 *prepared, VkBool32 *submitted, VkBool32 *accepted);
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
	status = queue_enqueue(owner, bindInfoCount, NULL, pBindInfo, fence, VK_TRUE, VK_FALSE);
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
	status = queue_enqueue(queue, count, submits, NULL, fence, VK_FALSE, VK_FALSE);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: ownership has passed to the actual GPU queue. */
	return VK_SUCCESS;
}

/*
 * Enqueues work while the caller retains queue ownership through job publication.
 */
VkResult
vulkan_queue_submit_locked(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	VkFence fence)
{
	VkResult status;

	/* Retain queue ownership through acceptance and return it after any pre-acceptance capacity wait. */
	status = queue_enqueue(queue, count, submits, NULL, fence, VK_FALSE, VK_TRUE);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the caller may atomically publish its already allocated presentation job. */
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

	/* The strict queue marker FIFO retires this job after all earlier committed jobs, including sparse work. */
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

/*
 * Releases cached native fences after the device has drained its accepted work.
 */
void
vulkan_queue_finish(
	struct VkQueue_T *queue)
{
	struct vulkan_queue_fence *slot;

	/* Device destruction is externally synchronized with every queue operation. */
	while (queue->private_fences != NULL) {
		/* Removes cache publication before callbacks may release its final native object. */
		slot = queue->private_fences;
		queue->private_fences = slot->next;

		/* Fence destruction waits for the strict host marker's final native access. */
		if (slot->fence != VK_NULL_HANDLE)
			vkDestroyFence((VkDevice)queue->device, slot->fence, NULL);

		/* The queue's device allocation policy owns every cache slot. */
		vulkan_free(&queue->device->object.allocator, slot);
	}

	/* Succeeded: the queue owns no hidden native fence or cache allocation. */
	return;
}

/* Enqueues only after capacity is available, dropping all producer locks while admission waits. */
static VkResult
queue_enqueue(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	const VkBindSparseInfo *binds,
	VkFence fence,
	VkBool32 sparse,
	VkBool32 locked)
{
	struct VkDevice_T *device;
	struct vulkan_queue_transaction transaction;
	struct vulkan_queue_fence *private_fence;
	struct vulkan_sync *completion;
	VkResult status;
	VkBool32 accepted;
	VkBool32 prepared;
	size_t capacity;
	uint64_t capacity_sequence;
	uint32_t request;
	uint32_t waits;
	uint64_t started;
	uint64_t mark;

	/* Counts temporary rollback storage before any native work or payload transition. */
	started = vulkan_timing_start();
	device = queue->device;
	capacity = 0;
	for (request = 0; request < count; request++) {
		/* Both ordinary and sparse transactions preserve the original wait-array order. */
		if (sparse != VK_FALSE) {
			waits = binds[request].waitSemaphoreCount;
		} else {
			waits = submits[request].waitSemaphoreCount;
		}

		/* A wrapping aggregate cannot safely represent the caller's binary payloads. */
		if (waits > SIZE_MAX - capacity)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Each occurrence reserves one possible proposed consumption entry. */
		capacity += waits;
	}

	/* Zero waits need no allocation and remain valid for idle and empty submissions. */
	memset(&transaction, 0, sizeof(transaction));
	if (capacity != 0) {
		/* Bounds the complete rollback array before invoking the application's allocator. */
		if (sizeof(*transaction.waits) > SIZE_MAX / capacity)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Allocation failure cannot occur after a kernel job has been reserved. */
		transaction.waits = vulkan_allocate(&device->object.allocator, capacity * sizeof(*transaction.waits), sizeof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
		if (transaction.waits == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* No proposed transition survives from an earlier capacity attempt. */
		memset(transaction.waits, 0, capacity * sizeof(*transaction.waits));
	}

	/* The host must finish borrowing a public completion proof before its next submission. */
	completion = vulkan_sync_object((uint64_t)(uintptr_t)fence);
	mark = vulkan_timing_start();
	vulkan_sync_quiesce(completion);
	vulkan_timing_add(VULKAN_TIMING_QUIESCE, mark, 0);
	private_fence = NULL;
	accepted = VK_FALSE;
	prepared = VK_FALSE;
	status = VK_SUCCESS;

	/* WSI callers already own queue serialization and receive it back on every return. */
	if (locked == VK_FALSE) {
		mark = vulkan_timing_start();
		pthread_mutex_lock(&queue->mutex);
		vulkan_timing_add(VULKAN_TIMING_QUEUE_LOCK, mark, 0);
	}

	/* A preparing private slot stays exclusive even while capacity waiting drops the queue lock. */
	if (fence == VK_NULL_HANDLE) {
		mark = vulkan_timing_start();
		status = queue_private_fence(queue, &private_fence);
		vulkan_timing_add(VULKAN_TIMING_PRIVATE_FENCE, mark, 0);
		if (status == VK_SUCCESS) {
			fence = private_fence->fence;
			completion = vulkan_sync_object((uint64_t)(uintptr_t)fence);
		}
	}

	/* Every retry takes a fresh capacity snapshot and rebuilds software payload observations. */
	while (status == VK_SUCCESS) {
		/* Native acceptance is the only operation allowed to consume proposed waits. */
		transaction.count = 0;
		status = queue_attempt(queue, count, submits, binds, fence, sparse, &transaction, completion, &accepted, &capacity_sequence, &prepared);
		if (status != VK_NOT_READY)
			break;

		/* Only RESERVE EAGAIN publishes an observed sequence authorizing capacity retry. */
		if (capacity_sequence == 0) {
			vulkan_device_error(device, VK_ERROR_DEVICE_LOST);
			vulkan_context_error(device->object.context, VK_ERROR_DEVICE_LOST);
			status = VK_ERROR_DEVICE_LOST;
			break;
		}

		/* Device and context locks were released by the unaccepted attempt before this queue unlock. */
		pthread_mutex_unlock(&queue->mutex);

		mark = vulkan_timing_start();
		status = vulkan_sync_capacity_wait(queue, capacity_sequence);
		vulkan_timing_add(VULKAN_TIMING_CAPACITY, mark, 0);

		pthread_mutex_lock(&queue->mutex);
	}

	/* A private slot belongs either to its admitted GPU job or to the next ordinary caller. */
	if (private_fence != NULL) {
		/* Accepted or uncertain work cannot expose its native proof to another preparation. */
		if (accepted != VK_FALSE || status == VK_ERROR_DEVICE_LOST) {
			private_fence->state = QUEUE_FENCE_INFLIGHT;
		} else {
			private_fence->state = QUEUE_FENCE_READY;
		}
	}

	/* A WSI caller retains its lock until it publishes the accepted presentation job. */
	if (locked == VK_FALSE)
		pthread_mutex_unlock(&queue->mutex);

	/* Temporary payload observations never outlive this synchronous API call. */
	vulkan_free(&device->object.allocator, transaction.waits);
	vulkan_timing_add(VULKAN_TIMING_SUBMIT, started, 0);
	vulkan_timing_report();
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: one native transaction consumed the final revalidated payload snapshot. */
	return VK_SUCCESS;
}

/* Tries one admission and native transaction while its caller owns queue serialization. */
static VkResult
queue_attempt(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	const VkBindSparseInfo *binds,
	VkFence fence,
	VkBool32 sparse,
	struct vulkan_queue_transaction *transaction,
	struct vulkan_sync *completion,
	VkBool32 *accepted,
	uint64_t *capacity_sequence,
	VkBool32 *prepared)
{
	struct VkDevice_T *device;
	struct vulkan_notification *reservation;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkResult ended;
	VkBool32 submitted;
	size_t index;
	uint32_t request;
	uint32_t replies;
	uint32_t opcode;
	uint64_t serial;
	uint64_t mark;

	/* Each attempt owns fresh wire and reply storage, with no reserved K job initially. */
	device = queue->device;
	reservation = NULL;
	*accepted = VK_FALSE;
	*capacity_sequence = 0;
	vulkan_writer_init_for_object(&writer, &queue->object);

	/* Rechecks shared payload state only while both queue and logical device are serialized. */
	mark = vulkan_timing_start();
	pthread_mutex_lock(&device->mutex);
	vulkan_timing_add(VULKAN_TIMING_DEVICE_LOCK, mark, 0);

	/*
	 * The command buffers whose End waits for this submission: their
	 * recordings lie first in the same transaction, once each, and their
	 * replies come before the submission's.
	 */
	serial = __atomic_add_fetch(&queue_end_serial, 1U, __ATOMIC_RELAXED);
	replies = 0U;
	if (sparse == VK_FALSE)
		queue_write_pending(&writer, count, submits, serial, &replies);

	/* Both native entrypoints use the same count and binary synchronization layout. */
	opcode = GPU_OP_QUEUE_SUBMIT;
	if (sparse != VK_FALSE)
		opcode = GPU_OP_QUEUE_BIND_SPARSE;
	vulkan_write_u32(&writer, opcode);
	vulkan_write_u32(&writer, GPU_OP_FLAG_REPLY);
	writer.opcode = opcode;

	/* A failure observed while capacity was unavailable invalidates this retry before submission. */
	status = vulkan_sync_device_status_locked(device);
	if (status != VK_SUCCESS)
		writer.error = status;

	/* Re-encodes caller waits rather than reusing an earlier software-signaled snapshot. */
	vulkan_write_u64(&writer, queue->object.wire_id);
	vulkan_write_u32(&writer, count);
	vulkan_write_u64(&writer, count);
	for (request = 0; request < count; request++) {
		/* Each successful attempt retains the requested native submission order. */
		if (sparse != VK_FALSE) {
			queue_write_sparse(&writer, transaction, &binds[request]);
		} else {
			queue_write_submit(&writer, transaction, &submits[request]);
		}
	}

	/* The exact proof and all marker ownership must exist before native acceptance. */
	queue_write_handle(&writer, (uint64_t)(uintptr_t)fence);
	status = writer.error;

	/*
	 * A kernel that takes the job, its stream and its commit in one call
	 * gets it so (one system call instead of three); a call it could not
	 * take that way ran nothing, and the three calls follow.
	 */
	submitted = VK_FALSE;
	if (status == VK_SUCCESS && sparse == VK_FALSE &&
	    (device->object.context->capabilities & GPU_CAP_JOB_SUBMIT) != 0U &&
	    device->object.context->sync_command != VK_FALSE) {
		status = queue_submit_job(queue, &writer, count, submits, serial, replies, completion, &reader, &ended, prepared,
					  &submitted, accepted);
	}
	if (submitted != VK_FALSE) {
		if (*accepted != VK_FALSE) {
			for (index = 0; index < transaction->count; index++) {
				if (transaction->waits[index].software != VK_FALSE)
					transaction->waits[index].sync->software_signaled = VK_FALSE;
			}
			completion->software_signaled = VK_FALSE;
			vulkan_content_commit(device, count, submits);
		}
		if (ended != VK_SUCCESS) {
			status = VK_ERROR_DEVICE_LOST;
			if (ended == VK_ERROR_OUT_OF_HOST_MEMORY || ended == VK_ERROR_OUT_OF_DEVICE_MEMORY)
				status = ended;
		}
		vulkan_sync_device_error(device, status);
		pthread_mutex_unlock(&device->mutex);
		vulkan_reader_finish(&reader);
		vulkan_writer_finish(&writer);
		return status;
	}
	if (status == VK_SUCCESS)
		status = vulkan_sync_job_reserve(queue, completion, &reservation, capacity_sequence, prepared);

	/* Backpressure leaves this attempt inert, preserving every caller payload and native fence. */
	if (status != VK_SUCCESS)
		writer.error = status;

	/* The decoded native result alone decides whether this transaction consumed its waits. */
	mark = vulkan_timing_start();
	ended = VK_SUCCESS;
	if (replies == 0U) {
		status = vulkan_command_execute(device->object.context, &writer, 8, &reader, VK_TRUE);
	} else {
		status = queue_execute_pending(device, &writer, count, submits, serial, replies, &reader, &ended);
	}
	vulkan_timing_add(VULKAN_TIMING_EXECUTE, mark, 0);
	if (status == VK_SUCCESS) {
		*accepted = VK_TRUE;

		/* Only accepted software waits stop representing immediate acquisition completion. */
		for (index = 0; index < transaction->count; index++) {
			/* Only software-complete payloads were omitted from the native wait array. */
			if (transaction->waits[index].software != VK_FALSE)
				transaction->waits[index].sync->software_signaled = VK_FALSE;
		}

		/* The new native job replaces any synchronous acquisition completion on this fence. */
		completion->software_signaled = VK_FALSE;

		/* Swapchain images the accepted command buffers write stop matching their earlier copies. */
		if (sparse == VK_FALSE)
			vulkan_content_commit(device, count, submits);
	}

	/*
	 * An out-of-memory answer to a submission of command buffers is no
	 * proof that none of their work reached the GPU (a renderer may have
	 * run the ones before the one that failed): the job ends as a fault,
	 * not a cancel (BUG-381).
	 */
	if ((status == VK_ERROR_OUT_OF_HOST_MEMORY || status == VK_ERROR_OUT_OF_DEVICE_MEMORY) && sparse == VK_FALSE &&
	    queue_has_commands(count, submits))
		status = VK_ERROR_DEVICE_LOST;

	/* Committed work retains K supervision; definite native refusal releases only its unused reservation. */
	mark = vulkan_timing_start();
	status = vulkan_sync_job_finish(queue, completion, reservation, status, *accepted);
	vulkan_timing_add(VULKAN_TIMING_FINISH, mark, 0);

	/*
	 * An ended recording the renderer failed is the submission's
	 * failure: out of memory as it is, any other as a lost device (the
	 * results vkQueueSubmit may return).
	 */
	if (ended != VK_SUCCESS) {
		status = VK_ERROR_DEVICE_LOST;
		if (ended == VK_ERROR_OUT_OF_HOST_MEMORY || ended == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			status = ended;
	}
	vulkan_sync_device_error(device, status);

	pthread_mutex_unlock(&device->mutex);

	/* Capacity retries discard this independent encoding before releasing the queue lock. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the kernel owns this exact native job independently of userspace progress. */
	return VK_SUCCESS;
}

/* Tells whether any of the submits carries a command buffer. */
static VkBool32
queue_has_commands(
	uint32_t count,
	const VkSubmitInfo *submits)
{
	uint32_t request;

	/* The first submit with one answers. */
	for (request = 0; request < count; request++) {
		if (submits[request].commandBufferCount != 0U)
			return VK_TRUE;
	}

	/* Succeeded: none carries one. */
	return VK_FALSE;
}

/*
 * Sends a submission's transaction as one supervised job (GPU_JOB_SUBMIT):
 * the completion's payload prepared, a ledger entry made, then the
 * transaction with the job; the kernel reserves, runs and commits or
 * cancels it.  submitted says the kernel took it (accepted that it was
 * committed); a job it did not take (no room, deferred) ran nothing and
 * leaves the reader empty for the three-call way.  Returns the
 * submission's result when submitted.
 */
static VkResult
queue_submit_job(
	struct VkQueue_T *queue,
	struct vulkan_writer *writer,
	uint32_t count,
	const VkSubmitInfo *submits,
	uint64_t serial,
	uint32_t replies,
	struct vulkan_sync *completion,
	struct vulkan_reader *reader,
	VkResult *ended,
	VkBool32 *prepared,
	VkBool32 *submitted,
	VkBool32 *accepted)
{
	struct VkDevice_T *device;
	struct vulkan_queue_job job;
	VkResult status;

	/* The payload's generation, prepared once for this submission. */
	device = queue->device;
	*submitted = VK_FALSE;
	*accepted = VK_FALSE;
	*ended = VK_SUCCESS;
	memset(&job, 0, sizeof(job));
	status = vulkan_external_fence_prepare_locked(device, completion, &job.fd, &job.generation, *prepared == VK_FALSE ? VK_TRUE : VK_FALSE);
	if (status != VK_SUCCESS) {
		*submitted = VK_TRUE;
		return status;
	}
	*prepared = VK_TRUE;

	/* The ledger entry the kernel's record will need. */
	job.notification = calloc(1, sizeof(*job.notification));
	if (job.notification == NULL) {
		*submitted = VK_TRUE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	job.timeline = queue->timeline_index;
	job.sync = completion;

	/* The transaction as the job. */
	writer->job = &job;
	if (replies == 0U) {
		status = vulkan_command_execute(device->object.context, writer, 8, reader, VK_TRUE);
	} else {
		status = queue_execute_pending(device, writer, count, submits, serial, replies, reader, ended);
	}
	writer->job = NULL;

	/* Not taken: nothing ran, the three calls follow. */
	if (job.outcome == 0U || job.outcome == GPU_JOB_SUBMIT_DEFERRED) {
		free(job.notification);
		vulkan_reader_finish(reader);
		vulkan_reader_init(reader, NULL, 0);
		*ended = VK_SUCCESS;
		return VK_SUCCESS;
	}

	/* Taken: committed work is accepted; a fault lost the context. */
	*submitted = VK_TRUE;
	if (job.outcome == GPU_JOB_SUBMIT_COMMITTED) {
		*accepted = VK_TRUE;
		return status;
	}
	if (job.outcome == GPU_JOB_SUBMIT_FAULTED)
		return VK_ERROR_DEVICE_LOST;

	/* Canceled: the submission's own result (a refusal), never success. */
	if (status == VK_SUCCESS)
		status = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	return status;
}

/*
 * Writes, ahead of the submission, the recordings of the submitted
 * command buffers whose End waits for it, in submission order, each
 * once, and counts the replies they ask for.
 */
static void
queue_write_pending(
	struct vulkan_writer *writer,
	uint32_t count,
	const VkSubmitInfo *submits,
	uint64_t serial,
	uint32_t *replies)
{
	struct VkCommandBuffer_T *command;
	uint32_t request;
	uint32_t index;

	/* Every command buffer of every submit, in order; a failure stays on its command buffer. */
	for (request = 0; request < count; request++) {
		for (index = 0; index < submits[request].commandBufferCount; index++) {
			command = (struct VkCommandBuffer_T *)submits[request].pCommandBuffers[index];
			(void)vulkan_command_pending_write(command, writer, serial, replies);
		}
	}

	/* Succeeded: the recordings lie ahead of the submission. */
	return;
}

/*
 * Runs a submission that carries ended recordings and reads its replies:
 * the recordings' (in the order queue_write_pending() wrote them, their
 * first failure in ended), then the submission's, whose result is
 * returned.
 */
static VkResult
queue_execute_pending(
	struct VkDevice_T *device,
	const struct vulkan_writer *writer,
	uint32_t count,
	const VkSubmitInfo *submits,
	uint64_t serial,
	uint32_t replies,
	struct vulkan_reader *reader,
	VkResult *ended)
{
	struct vulkan_context *context;
	struct VkCommandBuffer_T *command;
	uint32_t request;
	uint32_t index;
	uint32_t opcode;
	VkResult status;

	/* The transaction, with room for every reply. */
	context = device->object.context;
	*ended = VK_SUCCESS;
	status = vulkan_context_execute(context, writer, ((size_t)replies + 1U) * 8U, reader);
	if (status != VK_SUCCESS)
		return status;

	/* The recordings' replies, walking the command buffers as they were written. */
	for (request = 0; request < count; request++) {
		for (index = 0; index < submits[request].commandBufferCount; index++) {
			command = (struct VkCommandBuffer_T *)submits[request].pCommandBuffers[index];
			status = vulkan_command_pending_read(command, reader, serial);
			if (*ended == VK_SUCCESS)
				*ended = status;
		}
	}

	/* A reply out of order or cut short leaves the stream undecodable. */
	if (reader->error != VK_SUCCESS) {
		vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		return VK_ERROR_DEVICE_LOST;
	}

	/* The submission's own reply. */
	opcode = vulkan_read_u32(reader);
	if (reader->error != VK_SUCCESS || opcode != writer->opcode) {
		reader->error = VK_ERROR_DEVICE_LOST;
		vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		return VK_ERROR_DEVICE_LOST;
	}
	status = vulkan_read_result(reader);
	if (status == VK_ERROR_DEVICE_LOST)
		vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the submission was accepted. */
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

/* Borrows a prepared private proof while keeping other callers' admission and GPU ownership intact. */
static VkResult
queue_private_fence(
	struct VkQueue_T *queue,
	struct vulkan_queue_fence **result)
{
	struct vulkan_queue_fence *slot;
	struct vulkan_queue_fence *empty;
	VkFenceCreateInfo create;
	VkResult status;

	/* Previously batched resets can satisfy later submits without another native transaction. */
	for (slot = queue->private_fences; slot != NULL; slot = slot->next) {
		/* A prepared identity is free of both GPU and unaccepted producer ownership. */
		if (slot->state == QUEUE_FENCE_READY) {
			slot->state = QUEUE_FENCE_PREPARING;
			*result = slot;
			return VK_SUCCESS;
		}
	}

	/* Only exact terminal jobs contribute identities to the next native reset batch. */
	status = queue_reset_completed(queue);
	if (status != VK_SUCCESS)
		return status;

	/* Newly ready identities precede partial creations and avoid needless cache growth. */
	empty = NULL;
	for (slot = queue->private_fences; slot != NULL; slot = slot->next) {
		/* A prepared identity is free of both GPU and unaccepted producer ownership. */
		if (slot->state == QUEUE_FENCE_READY) {
			slot->state = QUEUE_FENCE_PREPARING;
			*result = slot;
			return VK_SUCCESS;
		}

		/* Failed first-time creation retains metadata without reserving it to any caller. */
		if (slot->state == QUEUE_FENCE_EMPTY)
			empty = slot;
	}

	/* Actual concurrent ownership, rather than frame count, grows the native proof cache. */
	slot = empty;
	if (slot == NULL) {
		slot = vulkan_allocate(&queue->device->object.allocator, sizeof(*slot), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
		if (slot == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* The queue owns partial construction until retry or device teardown. */
		memset(slot, 0, sizeof(*slot));
		slot->next = queue->private_fences;
		queue->private_fences = slot;
	}

	/* A first-use unsignaled fence needs no separate reset transaction. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence((VkDevice)queue->device, &create, NULL, &slot->fence);
	if (status != VK_SUCCESS)
		return status;

	/* PREPARING retains this identity even when its caller drops the queue lock to wait for admission. */
	slot->state = QUEUE_FENCE_PREPARING;
	*result = slot;

	/* Succeeded: the unsubmitted caller exclusively owns a native completion proof. */
	return VK_SUCCESS;
}

/* Resets one bounded batch of terminal identities without imposing a cache-size limit. */
static VkResult
queue_reset_completed(
	struct VkQueue_T *queue)
{
	struct vulkan_queue_fence *slots[QUEUE_FENCE_RESET_BATCH];
	VkFence fences[QUEUE_FENCE_RESET_BATCH];
	struct vulkan_queue_fence *slot;
	struct vulkan_sync *sync;
	uint32_t count;
	uint32_t cached;
	uint32_t index;
	VkResult status;

	/*
	 * The ledger, reaped once, tells which jobs ended: asking the kernel
	 * about each slot costs a system call a slot.  The reap consumes every
	 * ended record (each needs that once), so the batch takes about every
	 * slot whose job ended; one that ends after the reap waits for a later
	 * batch (the ledger is bounded, so is the cache).
	 */
	status = vulkan_sync_ledger_reap(queue->device->object.context);
	if (status != VK_SUCCESS)
		return status;

	/* Preparing callers and already-reset slots are not native GPU completion candidates. */
	count = 0U;
	cached = 0U;
	for (slot = queue->private_fences; slot != NULL; slot = slot->next) {
		/* The cache's size decides whether a small batch is worth its transaction. */
		cached++;

		/* A host marker's final native access must retire before that identity is reset. */
		if (slot->state == QUEUE_FENCE_INFLIGHT) {
			sync = vulkan_sync_object((uint64_t)(uintptr_t)slot->fence);
			status = vulkan_sync_job_retired(sync);
			if (status < VK_SUCCESS)
				return status;

			/* A pending job remains owned while independent completed jobs may join the batch. */
			if (status == VK_SUCCESS)
				slot->state = QUEUE_FENCE_TERMINAL;
		}

		/* No borrowed or already-prepared identity may enter a reset request. */
		if (slot->state != QUEUE_FENCE_TERMINAL)
			continue;

		/* One finite native request records each eligible identity exactly once. */
		slots[count] = slot;
		fences[count] = slot->fence;
		count++;
		if (count == QUEUE_FENCE_RESET_BATCH)
			break;
	}

	/* No terminal work means that another exclusive caller needs a new native identity. */
	if (count == 0U)
		return VK_SUCCESS;

	/* A small batch waits for more ended jobs while the cache may still grow; the caller makes a fence. */
	if (count < QUEUE_FENCE_RESET_MIN && cached < QUEUE_FENCE_CACHE_MAX)
		return VK_SUCCESS;

	/* One native transaction resets all selected proofs after their final marker accesses ended. */
	status = vkResetFences((VkDevice)queue->device, count, fences);
	if (status != VK_SUCCESS)
		return status;

	/* Only successful reset publishes identities reusable without another native round trip. */
	for (index = 0U; index < count; index++)
		slots[index]->state = QUEUE_FENCE_READY;

	/* Succeeded: later submissions can independently borrow this batch's reset proofs. */
	return VK_SUCCESS;
}
