/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete native retirement precedes the QueueSubmit reply; common producer markers remain separate FIFO work. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/vulkan-queue.h"
#include "drivers/gpu/bcm2711/vulkan-device.h"
#include "drivers/gpu/bcm2711/vulkan-sync.h"
#include "drivers/gpu/bcm2711/vulkan-native-job.h"

/* Submission metadata and prepared snapshots have independent finite bounds and never occupy the kernel stack. */
#define QUEUE_BATCHES 64U
#define QUEUE_ACTIONS 256U
#define QUEUE_PREPARED_BYTES (8U * 1024U * 1024U)
#define QUEUE_STAGE_MASK (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)

/* One action represents an ordered binary wait, complete pending primary or binary signal inside a controller-serialized submission. */
enum queue_action_kind {
	QUEUE_WAIT,
	QUEUE_COMMAND,
	QUEUE_SIGNAL
};

/* Decoded identities are numerical arena storage used only during this synchronous command, without application pointers. */
struct queue_batch {
	uint32_t waits;
	uint32_t commands;
	uint32_t signals;
	uint64_t *wait_ids;
	uint64_t *command_ids;
	uint64_t *signal_ids;
	uint32_t *stages;
};

/* Each action borrows a separately retained sync edge or owns one entire independently pending primary job until exact disposal. */
struct queue_action {
	enum queue_action_kind kind;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_native_job *job;
};

/* One distinct binary payload has a private proposed operation sequence until the whole submission passes preparation. */
struct queue_sync {
	struct bcm2711_vulkan_object *object;
	uint64_t signals;
	uint64_t waits;
};

/* The complete command-arena submission owns all fallible preparations before any pixel, layout, fence or semaphore changes. */
struct queue_submission {
	struct bcm2711_vulkan_object *queue;
	struct bcm2711_vulkan_object *fence;
	struct bcm2711_vulkan_object *device;
	struct queue_batch batches[QUEUE_BATCHES];
	struct queue_action actions[QUEUE_ACTIONS];
	struct queue_sync syncs[QUEUE_ACTIONS];
	uint32_t count;
	uint32_t action_count;
	uint32_t sync_count;
	uint32_t bytes;
};

static int check_admission(struct bcm2711_vulkan_session *session);
static int decode_submission(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct queue_submission *submission);
static int decode_batch(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct queue_batch *batch);
static int decode_ids(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, uint32_t *count, uint64_t **ids);
static int prepare_submission(struct bcm2711_vulkan_session *session, struct queue_submission *submission);
static int prepare_sync(struct bcm2711_vulkan_session *session, struct queue_submission *submission, uint64_t identity, enum queue_action_kind kind);
static int prepare_command(struct bcm2711_vulkan_session *session, struct queue_submission *submission, uint64_t identity);
static int run_submission(struct bcm2711_vulkan_session *session, struct queue_submission *submission);
static int release_submission(struct bcm2711_vulkan_session *session, struct queue_submission *submission);
static void publish_failure(struct bcm2711_vulkan_session *session, struct queue_submission *submission);

/*
 * Executes one complete QueueSubmit on the ordered command worker with all-or-nothing preflight and explicit native retirement.
 */
int
bcm2711_vulkan_queue_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct queue_submission *submission;
	VkResult status;
	int error;
	int released;

	/* This graphics-only route makes no sparse, timeline or submit2 capability claim. */
	*handled = 0;
	if (opcode != GPU_OP_QUEUE_SUBMIT)
		return 0;
	*handled = 1;
	if (requested != 1)
		return EINVAL;

	/* Even empty or signal-only work cannot resurrect a stopped or uncertain native controller. */
	error = check_admission(session);
	if (error != 0)
		return error;

	/* Bounded session-arena metadata stays off the kernel stack and requires no fallible allocation before complete wire consumption. */
	submission = i915_vkc_array(reader, &session->arena, 1, sizeof(*submission));
	if (submission == NULL)
		return EINVAL;
	error = decode_submission(session, reader, submission);
	if (error == 0)
		error = prepare_submission(session, submission);
	if (error != 0) {
		released = release_submission(session, submission);
		if (released != 0)
			return released;
		if (error == ENOMEM) {
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
			return 0;
		}

		/* Complete malformed or unsupported ownership refusal remains visible without any partial execution. */
		return error;
	}

	/* The already ordered command worker executes every complete primary before decoder completion acknowledges this submit. */
	error = run_submission(session, submission);
	status = VK_SUCCESS;
	if (error != 0) {
		publish_failure(session, submission);
		status = VK_ERROR_DEVICE_LOST;
	}

	/* Whole uncertain primary jobs transfer to controller quarantine before sync metadata or common callbacks retire. */
	released = release_submission(session, submission);
	if (released != 0) {
		bcm2711_render_worker_fault(session->render->device, released);
		status = VK_ERROR_DEVICE_LOST;
	}

	/* Only terminal native retirement or sticky device loss becomes a submit result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);

	/* Succeeded: the Vulkan result reflects actual work retirement or sticky native loss, never decoder progress alone. */
	return 0;
}

/* Samples IRQ-published native fault and stop state before any queue can publish a new fence or binary operation. */
static int
check_admission(
	struct bcm2711_vulkan_session *session)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;
	int error;

	/* The controller mutex protects the session and power owner while the IRQ guard stabilizes stop and fault publication. */
	controller = session->render->device;
	hardware = &controller->space.native->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	error = 0;
	if (session->closing ||
	    session->render->stopping ||
	    controller->worker.uncertain ||
	    hardware->faulted ||
	    !hardware->ready ||
	    !hardware->initialized ||
	    !controller->space.native->power.ready)
		error = EIO;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* A closed or uncertain controller supplies no new native execution or completion proof. */
	if (error != 0)
		return error;

	/* Succeeded: this exact controller may accept one complete ordered native submission. */
	return 0;
}

/* Decodes all complete batch metadata and the trailing optional fence before retaining any queue or resource. */
static int
decode_submission(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct queue_submission *submission)
{
	struct bcm2711_vulkan_object *queue;
	struct bcm2711_vulkan_root *root;
	struct bcm2711_vulkan_object *fence;
	struct bcm2711_vulkan_sync *sync;
	uint64_t identity;
	uint64_t array;
	uint32_t index;
	int error;

	/* Queue identity and exact array cardinality precede every selected submit record. */
	identity = drv_i915_wire_read_u64(reader);
	queue = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUEUE, identity);
	submission->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    queue == NULL ||
	    queue->payload == NULL ||
	    submission->count > QUEUE_BATCHES ||
	    array != submission->count)
		return EINVAL;
	root = queue->payload;
	if (root->parent == NULL || root->parent->kind != I915_VK_OBJ_DEVICE)
		return EINVAL;
	submission->device = root->parent;
	for (index = 0; index < submission->count; index++) {
		error = decode_batch(session, reader, &submission->batches[index]);
		if (error != 0)
			return error;
	}

	/* The optional trailing fence belongs to the same logical device and cannot replace an existing pending or signaled operation. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	fence = NULL;
	if (identity != 0) {
		fence = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FENCE, identity);
		if (fence == NULL || fence->payload == NULL)
			return EINVAL;
		sync = fence->payload;
		if (sync->device != submission->device ||
		    sync->pending ||
		    sync->signaled ||
		    sync->status != VK_SUCCESS)
			return EINVAL;
	}

	/* Independent roots are retained only after the complete input and trailing fence passed preflight. */
	error = bcm2711_vulkan_object_retain(queue);
	if (error != 0)
		return error;
	submission->queue = queue;
	if (fence != NULL) {
		error = bcm2711_vulkan_object_retain(fence);
		if (error != 0)
			return error;
		submission->fence = fence;
	}

	/* Succeeded: complete numerical batches and retained exact queue/fence roots are ready for CPU-only preparation. */
	return 0;
}

/* Decodes the exact legacy client submit shape, including independently framed stage masks and zero extension chain. */
static int
decode_batch(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct queue_batch *batch)
{
	uint32_t type;
	uint64_t extension;
	uint64_t array;
	uint32_t index;
	int error;

	/* This client sends no native submit extension record in the selected binary-only Vulkan profile. */
	type = drv_i915_wire_read_u32(reader);
	extension = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || type != VK_STRUCTURE_TYPE_SUBMIT_INFO)
		return EINVAL;
	if (extension != 0)
		return ENOTSUP;
	error = decode_ids(session, reader, &batch->waits, &batch->wait_ids);
	if (error != 0)
		return error;

	/* Stage cardinality exactly matches the already decoded native wait vector. */
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != batch->waits)
		return EINVAL;
	if (batch->waits != 0) {
		batch->stages = i915_vkc_array(reader, &session->arena, batch->waits, sizeof(*batch->stages));
		if (batch->stages == NULL)
			return EINVAL;
		for (index = 0; index < batch->waits; index++) {
			batch->stages[index] = drv_i915_wire_read_u32(reader);
		}
	}

	/* Commands and signals keep their exact independent declared and payload lengths. */
	error = decode_ids(session, reader, &batch->commands, &batch->command_ids);
	if (error != 0)
		return error;
	error = decode_ids(session, reader, &batch->signals, &batch->signal_ids);
	if (error != 0)
		return error;

	/* Succeeded: one whole numerical batch was copied into bounded command-local CPU storage. */
	return 0;
}

/* Copies one exact bounded numerical identity vector without treating wire integers as application pointers. */
static int
decode_ids(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	uint32_t *count,
	uint64_t **ids)
{
	uint64_t array;
	uint32_t index;

	/* Complete count and payload cardinality must agree before any arena allocation or array access. */
	*ids = NULL;
	*count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    *count > QUEUE_ACTIONS ||
	    array != *count)
		return EINVAL;
	if (*count == 0)
		return 0;
	*ids = i915_vkc_array(reader, &session->arena, *count, sizeof(**ids));
	if (*ids == NULL)
		return EINVAL;
	for (index = 0; index < *count; index++) {
		(*ids)[index] = drv_i915_wire_read_u64(reader);
	}

	/* Truncated complete input acquires no typed owner or synchronization transition. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: every declared identity is independently copied into this command's bounded arena. */
	return 0;
}

/* Prepares every primary and proposes every binary transition before a first output or live sync state changes. */
static int
prepare_submission(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission)
{
	struct queue_batch *batch;
	uint32_t index;
	uint32_t item;
	int error;

	/* Waits precede each batch's ordered work, and signals follow confirmed completion of that work. */
	for (index = 0; index < submission->count; index++) {
		batch = &submission->batches[index];
		for (item = 0; item < batch->waits; item++) {
			if (batch->stages[item] == 0 || (batch->stages[item] & ~QUEUE_STAGE_MASK) != 0)
				return ENOTSUP;
			error = prepare_sync(session, submission, batch->wait_ids[item], QUEUE_WAIT);
			if (error != 0)
				return error;
		}

		/* All independently pending CPU primary snapshots precede any live fence, semaphore, image or layout mutation. */
		for (item = 0; item < batch->commands; item++) {
			error = prepare_command(session, submission, batch->command_ids[item]);
			if (error != 0)
				return error;
		}

		/* Complete following signals are validated against prior proposed waits across every batch. */
		for (item = 0; item < batch->signals; item++) {
			error = prepare_sync(session, submission, batch->signal_ids[item], QUEUE_SIGNAL);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the whole finite submission owns complete CPU snapshots and consistent private binary proposals. */
	return 0;
}

/* Retains each distinct same-device binary semaphore and records one proposed signal or consumption with no live mutation. */
static int
prepare_sync(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission,
	uint64_t identity,
	enum queue_action_kind kind)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_sync *sync;
	struct queue_sync *shadow;
	struct queue_action *action;
	uint32_t index;
	int error;

	/* Finite action count includes waits, primaries and signals together rather than bounding each vector independently. */
	if (submission->action_count == QUEUE_ACTIONS)
		return ENOMEM;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SEMAPHORE, identity);
	if (object == NULL || object->payload == NULL)
		return EINVAL;
	sync = object->payload;
	if (sync->device != submission->device ||
	    sync->pending ||
	    sync->status != VK_SUCCESS ||
	    sync->signals_reserved != sync->signals_completed ||
	    sync->waits_reserved != sync->waits_completed)
		return EINVAL;

	/* Binary native sequence and observable payload state must agree before proposing any additional operation. */
	if (sync->waits_reserved > sync->signals_reserved ||
	    sync->signals_reserved - sync->waits_reserved > 1)
		return EINVAL;
	if (sync->signaled) {
		if (sync->signals_reserved == sync->waits_reserved)
			return EINVAL;
	} else {
		if (sync->signals_reserved != sync->waits_reserved)
			return EINVAL;
	}

	/* A distinct payload acquires exactly one independent edge regardless of the number of operations in this complete transaction. */
	shadow = NULL;
	for (index = 0; index < submission->sync_count; index++) {
		if (submission->syncs[index].object == object) {
			shadow = &submission->syncs[index];
			break;
		}
	}

	/* Previously proposed operations remain private until all following commands and signals passed preparation. */
	if (shadow == NULL) {
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0)
			return error;
		shadow = &submission->syncs[submission->sync_count];
		shadow->object = object;
		shadow->signals = sync->signals_reserved;
		shadow->waits = sync->waits_reserved;
		submission->sync_count++;
	}

	/* Binary waits consume one already submitted signal; signals require its earlier payload to have been consumed. */
	if (kind == QUEUE_WAIT) {
		if (shadow->waits == shadow->signals)
			return EINVAL;
		shadow->waits++;
	} else {
		if (shadow->signals != shadow->waits)
			return EINVAL;
		if (shadow->signals == UINT64_MAX)
			return EOVERFLOW;
		shadow->signals++;
	}

	/* Only the complete private transition enters the ordered owned action vector. */
	action = &submission->actions[submission->action_count];
	action->kind = kind;
	action->object = object;
	submission->action_count++;

	/* Succeeded: one independently retained binary action joins the complete unpublished submission. */
	return 0;
}

/* Acquires one complete same-device pending primary and charges aggregate CPU snapshots before publication or native work. */
static int
prepare_command(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission,
	uint64_t identity)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct queue_action *action;
	int error;

	/* The exact native primary belongs to the sole graphics family through its actual same-device pool. */
	if (submission->action_count == QUEUE_ACTIONS)
		return ENOMEM;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL || object->payload == NULL)
		return EINVAL;
	command = object->payload;
	if (command->owner.device != submission->device)
		return EINVAL;

	/* Complete construction owns its pending primary graph before aggregate budget refusal can unwind it. */
	action = &submission->actions[submission->action_count];
	action->kind = QUEUE_COMMAND;
	action->object = object;
	error = bcm2711_vulkan_native_job_create(object, &action->job);
	if (error != 0)
		return error;
	submission->action_count++;
	if (action->job->prepared->bytes > QUEUE_PREPARED_BYTES - submission->bytes)
		return ENOMEM;
	submission->bytes += action->job->prepared->bytes;

	/* Succeeded: one complete pending CPU graph remains independently owned without a native allocation or launch. */
	return 0;
}

/* Publishes reserved native sync state and executes ordered complete actions only after whole preparation succeeded. */
static int
run_submission(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission)
{
	struct bcm2711_vulkan_sync *sync;
	struct bcm2711_vulkan_command_buffer *command;
	struct queue_action *action;
	uint32_t index;
	bool retired;
	int error;
	int disposed;

	/* Each exact binary payload now owns its complete proposed operation sequence until this synchronous worker command retires. */
	for (index = 0; index < submission->sync_count; index++) {
		sync = submission->syncs[index].object->payload;
		sync->signals_reserved = submission->syncs[index].signals;
		sync->waits_reserved = submission->syncs[index].waits;
		sync->pending = true;
	}

	/* The fence cannot be reset or observed completed before all preceding actions truly retire. */
	if (submission->fence != NULL) {
		sync = submission->fence->payload;
		sync->pending = true;
	}

	/* Strict native retirement supplies stronger ordering than each supported wait stage, without a second worker or hidden completion. */
	for (index = 0; index < submission->action_count; index++) {
		action = &submission->actions[index];
		if (action->kind == QUEUE_COMMAND) {
			error = bcm2711_vulkan_native_job_execute(session->render->device, session->render, action->job, &retired);
			command = action->object->payload;
			if (action->job->executed && (command->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0)
				command->state = BCM2711_VULKAN_COMMAND_INVALID;
			if (error != 0)
				return error;

			/* A nominal runner success without actual DMA retirement can never signal this submission's fence. */
			if (!retired)
				return EIO;
			disposed = bcm2711_vulkan_native_job_dispose(session->render->device, action->job, retired);
			action->job = NULL;
			if (disposed != 0)
				return disposed;
		} else {
			/* Actual completed binary counters advance only at their ordered wait or signal execution point. */
			sync = action->object->payload;
			if (action->kind == QUEUE_WAIT) {
				sync->waits_completed++;
				sync->signaled = false;
			} else {
				sync->signals_completed++;
				sync->signaled = true;
			}
		}
	}

	/* Every binary payload has completed the exact finite reserved sequence before it becomes reusable. */
	for (index = 0; index < submission->sync_count; index++) {
		sync = submission->syncs[index].object->payload;
		sync->pending = false;
	}

	/* Only full successful native and metadata retirement signals the optional fence, including an empty submit after preceding FIFO work. */
	if (submission->fence != NULL) {
		sync = submission->fence->payload;
		sync->pending = false;
		sync->signaled = true;
	}

	/* Succeeded: every complete primary, wait and signal retired in order before the submit acknowledgement. */
	return 0;
}

/* Retains uncertain whole primary roots in the controller and retires every independent CPU synchronization and queue edge. */
static int
release_submission(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission)
{
	struct queue_action *action;
	uint32_t index;
	int first;
	int error;

	/* Unlaunched and confirmed retired jobs release normally; an uncertain DMA root transfers without allocation to controller quarantine. */
	first = 0;
	for (index = 0; index < submission->action_count; index++) {
		action = &submission->actions[index];
		if (action->job == NULL)
			continue;
		error = bcm2711_vulkan_native_job_dispose(session->render->device, action->job, action->job->retired);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Distinct binary CPU payloads retire after all native roots independently preserved their pending primary and session owners. */
	for (index = 0; index < submission->sync_count; index++) {
		error = bcm2711_vulkan_object_release(submission->syncs[index].object);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Optional fence and queue edges retire last while the complete CPU root remains the unwind authority. */
	error = bcm2711_vulkan_object_release(submission->fence);
	if (first == 0 && error != 0)
		first = error;
	error = bcm2711_vulkan_object_release(submission->queue);
	if (first == 0 && error != 0)
		first = error;
	if (first != 0)
		return first;

	/* Succeeded: no CPU prefix lost an independent native owner, logical parent or quarantine root. */
	return 0;
}

/* Publishes sticky loss before any failed submission can retire a callback or report its fence complete. */
static void
publish_failure(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission)
{
	struct bcm2711_vulkan_sync *sync;
	uint32_t index;

	/* Native admission closes before uncertain jobs transfer to the persistent controller owner. */
	bcm2711_render_worker_fault(session->render->device, EIO);
	for (index = 0; index < submission->sync_count; index++) {
		sync = submission->syncs[index].object->payload;
		sync->status = VK_ERROR_DEVICE_LOST;
		sync->pending = false;
	}

	/* A failed fence remains unsignaled with sticky loss even when preceding actions already modified images or consumed waits. */
	if (submission->fence != NULL) {
		sync = submission->fence->payload;
		sync->status = VK_ERROR_DEVICE_LOST;
		sync->pending = false;
		sync->signaled = false;
	}

	/* Succeeded: device loss precedes all subsequent payload and common marker observations. */
	return;
}
