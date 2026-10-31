/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A prepared QueueSubmit is replied to at once and runs later on the executor; a common marker completes after it. */
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/vk/vulkan-queue.h"
#include "drivers/gpu/bcm2711/vk/vulkan-device.h"
#include "drivers/gpu/bcm2711/vk/vulkan-sync.h"
#include "drivers/gpu/bcm2711/vk/vulkan-native-job.h"
#include "drivers/gpu/bcm2711/vk/vulkan-prepared.h"
#include "drivers/gpu/bcm2711/vk/vulkan-record.h"
#include "drivers/gpu/bcm2711/vk/vulkan-target.h"
#include "drivers/gpu/bcm2711/vk/vulkan-input.h"
#include "drivers/gpu/bcm2711/vk/vulkan-resource.h"
#include "drivers/gpu/bcm2711/vk/vulkan-memory.h"
#include "drivers/gpu/bcm2711/buffer.h"
#if CONFIG_DRIVER_BCM2711_VIDEO
#include "drivers/gpu/bcm2711/vk/vulkan-command.h"
#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#endif

/* Submission metadata and prepared snapshots have independent finite bounds and never occupy the kernel stack. */
#define QUEUE_BATCHES 64U
#define QUEUE_ACTIONS 256U
#define QUEUE_PREPARED_BYTES BCM2711_RENDER_SUBMISSION_BYTES
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

/* The retained queue and fence, the ordered actions and the distinct binary payloads one submission owns. */
struct queue_owner {
	struct bcm2711_vulkan_object *queue;
	struct bcm2711_vulkan_object *fence;
	struct queue_action *actions;
	struct queue_sync *syncs;
	uint32_t action_count;
	uint32_t sync_count;
};

/* The complete command-arena submission owns all fallible preparations before any pixel, layout, fence or semaphore changes. */
struct queue_submission {
	struct queue_owner owner;
	struct bcm2711_vulkan_object *device;
	struct queue_batch batches[QUEUE_BATCHES];
	struct queue_action actions[QUEUE_ACTIONS];
	struct queue_sync syncs[QUEUE_ACTIONS];
	uint32_t count;
	uint32_t bytes;
};

/*
 * An accepted submission in its session's execution queue: a heap copy of
 * the owned actions and payloads, and the next action to run.  The arrays
 * follow the structure in the same allocation.  With video, mark holds the
 * decodes in flight its fence waits for once its actions have run.
 */
struct queue_execution {
	struct bcm2711_render_execution base;
	struct bcm2711_vulkan_session *session;
	struct queue_owner owner;
	uint32_t cursor;
#if CONFIG_DRIVER_BCM2711_VIDEO
	struct bcm2711_vulkan_video_mark mark;
#endif
};

static int check_admission(struct bcm2711_vulkan_session *session);
static int decode_submission(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct queue_submission *submission);
static int decode_batch(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct queue_batch *batch);
static int decode_ids(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, uint32_t *count, uint64_t **ids);
static int prepare_submission(struct bcm2711_vulkan_session *session, struct queue_submission *submission);
static int prepare_sync(struct bcm2711_vulkan_session *session, struct queue_submission *submission, uint64_t identity, enum queue_action_kind kind);
static int prepare_command(struct bcm2711_vulkan_session *session, struct queue_submission *submission, uint64_t identity);
static struct queue_execution *create_execution(struct bcm2711_vulkan_session *session, struct queue_submission *submission);
static void publish_reservations(struct queue_owner *owner);
static bool owner_displayed(const struct queue_owner *owner);
static bool object_displayed(const struct bcm2711_vulkan_object *object);
static int execution_step(struct bcm2711_render_device *controller, struct bcm2711_render_execution *base, bool *done);
static int execution_end(struct bcm2711_render_device *controller, struct bcm2711_render_execution *base, int error);
#if CONFIG_DRIVER_BCM2711_VIDEO
static void execution_mark(struct queue_execution *execution);
static int execution_settle(struct bcm2711_render_device *controller, struct bcm2711_render_execution *base, bool wait);
#endif
static void publish_completion(struct queue_owner *owner);
static void publish_failure(struct queue_owner *owner);
static int release_owner(struct bcm2711_vulkan_session *session, struct queue_owner *owner);

/*
 * Accepts one complete QueueSubmit with all-or-nothing preflight and queues it for the executor.
 *
 * The reply reports acceptance, not completion: the binary payloads and the
 * fence publish their reserved operations now and their completion or loss
 * when the execution ends, before a later common marker of the session.
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
	struct queue_execution *execution;
	int error;
	int released;

	/* This graphics-only route makes no sparse, timeline or submit2 capability claim. */
	*handled = 0;
	if (opcode != GPU_OP_QUEUE_SUBMIT)
		return 0;
	*handled = 1;
	if (requested != 1)
		return EINVAL;

	/* A synchronous legacy frame has no callback to complete after the execution. */
	if (session->synchronous)
		return EOPNOTSUPP;

	/* Even empty or signal-only work cannot resurrect a stopped or uncertain native controller. */
	error = check_admission(session);
	if (error != 0)
		return error;

	/* Bounded session-arena metadata stays off the kernel stack and requires no fallible allocation before complete wire consumption. */
	submission = i915_vkc_array(reader, &session->arena, 1, sizeof(*submission));
	if (submission == NULL)
		return EINVAL;
	submission->owner.actions = submission->actions;
	submission->owner.syncs = submission->syncs;
	error = decode_submission(session, reader, submission);
	if (error == 0)
		error = prepare_submission(session, submission);

	/* A stream past the hard limits is refused; the execution's heap copy is the last fallible step before publication. */
	execution = NULL;
	if (error == 0)
		error = bcm2711_render_execution_admit(session->render, submission->bytes);
	if (error == 0) {
		execution = create_execution(session, submission);
		if (execution == NULL)
			error = ENOMEM;
	}
	if (error != 0) {
		released = release_owner(session, &submission->owner);
		if (released != 0)
			return released;
		if (error == ENOMEM) {
			drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
			return 0;
		}

		/* Complete malformed or unsupported ownership refusal remains visible without any partial execution. */
		return error;
	}

	/* The reserved operations are visible before the execution can run or a later submission can check them. */
	publish_reservations(&execution->owner);
	bcm2711_render_execution_submit(session->render, &execution->base);

	/* The reply acknowledges acceptance; completion and loss travel through the payloads and the session's markers. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);

	/* Succeeded: the execution owns every prepared job and retained payload until it ends. */
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
	const char *reason;

	/* The controller mutex protects the session and power owner while the IRQ guard stabilizes stop and fault publication. */
	controller = session->render->device;
	hardware = &controller->space.native->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	reason = NULL;
	if (session->closing) {
		reason = "the Vulkan session is closing";
	} else if (session->render->stopping) {
		reason = "the session is stopping";
	} else if (controller->worker.uncertain) {
		reason = "the engine is uncertain after a fault";
	} else if (hardware->faulted) {
		reason = "the engine faulted";
	} else if (!hardware->ready) {
		reason = "the engine is not ready";
	} else if (!hardware->initialized) {
		reason = "the engine is not initialized";
	} else if (!controller->space.native->power.ready) {
		reason = "the engine's power is not ready";
	}

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* A closed or uncertain controller supplies no new native execution or completion proof; the refusal says why. */
	if (reason != NULL) {
		bcm2711_render_refusal_line("queue submit", reason);
		return EIO;
	}

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
	submission->owner.queue = queue;
	if (fence != NULL) {
		error = bcm2711_vulkan_object_retain(fence);
		if (error != 0)
			return error;
		submission->owner.fence = fence;
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
	if (submission->owner.action_count == QUEUE_ACTIONS)
		return ENOMEM;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SEMAPHORE, identity);
	if (object == NULL || object->payload == NULL)
		return EINVAL;
	sync = object->payload;
	if (sync->device != submission->device || sync->status != VK_SUCCESS)
		return EINVAL;

	/*
	 * New operations follow the reserved ones, which may still wait for an
	 * earlier execution: a wait may consume a signal that is only reserved.
	 * Without pending operations the observable state must agree too.
	 */
	if (sync->waits_reserved > sync->signals_reserved ||
	    sync->signals_reserved - sync->waits_reserved > 1)
		return EINVAL;
	if (sync->pending == 0) {
		if (sync->signals_reserved != sync->signals_completed || sync->waits_reserved != sync->waits_completed)
			return EINVAL;
		if (sync->signaled) {
			if (sync->signals_reserved == sync->waits_reserved)
				return EINVAL;
		} else {
			if (sync->signals_reserved != sync->waits_reserved)
				return EINVAL;
		}
	}

	/* A distinct payload acquires exactly one independent edge regardless of the number of operations in this complete transaction. */
	shadow = NULL;
	for (index = 0; index < submission->owner.sync_count; index++) {
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
		shadow = &submission->syncs[submission->owner.sync_count];
		shadow->object = object;
		shadow->signals = sync->signals_reserved;
		shadow->waits = sync->waits_reserved;
		submission->owner.sync_count++;
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
	action = &submission->actions[submission->owner.action_count];
	action->kind = kind;
	action->object = object;
	submission->owner.action_count++;

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
	if (submission->owner.action_count == QUEUE_ACTIONS)
		return ENOMEM;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL || object->payload == NULL)
		return EINVAL;
	command = object->payload;
	if (command->owner.device != submission->device)
		return EINVAL;

	/* Complete construction owns its pending primary graph before aggregate budget refusal can unwind it. */
	action = &submission->actions[submission->owner.action_count];
	action->kind = QUEUE_COMMAND;
	action->object = object;
	error = bcm2711_vulkan_native_job_create(object, &action->job);
	if (error != 0)
		return error;
	submission->owner.action_count++;
	if (action->job->prepared->bytes > QUEUE_PREPARED_BYTES - submission->bytes)
		return ENOMEM;
	submission->bytes += action->job->prepared->bytes;

	/* Succeeded: one complete pending CPU graph remains independently owned without a native allocation or launch. */
	return 0;
}

/* Copies the submission's owned actions and payloads into one heap execution; NULL when memory is short. */
static struct queue_execution *
create_execution(
	struct bcm2711_vulkan_session *session,
	struct queue_submission *submission)
{
	struct queue_execution *execution;
	struct bcm2711_vulkan_root *root;
	size_t bytes;

	/* The arrays follow the structure; pointers keep their natural alignment. */
	bytes = sizeof(*execution);
	bytes += (size_t)submission->owner.action_count * sizeof(struct queue_action);
	bytes += (size_t)submission->owner.sync_count * sizeof(struct queue_sync);
	execution = kern_calloc(1, bytes);
	if (execution == NULL)
		return NULL;

	/* Ownership moves without another reference: the arena copy is never released. */
	execution->owner.actions = (struct queue_action *)(execution + 1);
	execution->owner.syncs = (struct queue_sync *)(execution->owner.actions + submission->owner.action_count);
	kern_memcpy(execution->owner.actions, submission->actions, (size_t)submission->owner.action_count * sizeof(struct queue_action));
	kern_memcpy(execution->owner.syncs, submission->syncs, (size_t)submission->owner.sync_count * sizeof(struct queue_sync));
	execution->owner.action_count = submission->owner.action_count;
	execution->owner.sync_count = submission->owner.sync_count;
	execution->owner.queue = submission->owner.queue;
	execution->owner.fence = submission->owner.fence;
	execution->session = session;
	execution->base.step = execution_step;
	execution->base.end = execution_end;
	execution->base.bytes = submission->bytes;
	execution->base.display = owner_displayed(&execution->owner);

	/* The queue's domain, whose reserved marker completes the submission. */
	root = submission->owner.queue->payload;
	execution->base.timeline = root->timeline;

	/* Succeeded: the execution owns everything the submission prepared. */
	return execution;
}

/*
 * Tells whether a submission's commands use a run the display holds.
 *
 * Every object the recorded passes and transfers name counts, read or
 * written: a frame's target, a present copy's destination, the cursor image.
 */
static bool
owner_displayed(
	const struct queue_owner *owner)
{
	const struct bcm2711_vulkan_prepared_event *event;
	const struct bcm2711_vulkan_record *records[2];
	uint32_t index;
	uint32_t record;
	uint32_t object;

	/* Each command's prepared events and the pass each belongs to. */
	for (index = 0; index < owner->action_count; index++) {
		if (owner->actions[index].kind != QUEUE_COMMAND)
			continue;
		for (event = owner->actions[index].job->prepared->first; event != NULL; event = event->next) {
			records[0] = event->record;
			records[1] = event->pass;
			for (record = 0; record < 2; record++) {
				if (records[record] == NULL)
					continue;
				for (object = 0; object < BCM2711_VULKAN_VERTEX_BINDINGS + 1U; object++) {
					if (object_displayed(records[record]->objects[object]))
						return true;
				}
			}
		}
	}

	/* Succeeded: no displayed run is used. */
	return false;
}

/* Tells whether an image, buffer, image view or framebuffer is backed by a run the display holds. */
static bool
object_displayed(
	const struct bcm2711_vulkan_object *object)
{
	const struct bcm2711_vulkan_framebuffer *framebuffer;
	const struct bcm2711_vulkan_image_view *view;
	const struct bcm2711_vulkan_resource *resource;
	const struct bcm2711_vulkan_memory *memory;
	struct bcm2711_buffer *buffer;
	unsigned long enabled;
	bool displayed;

	/* A framebuffer names its view, and a view its image. */
	if (object == NULL || object->payload == NULL)
		return false;
	if (object->kind == I915_VK_OBJ_FRAMEBUFFER) {
		framebuffer = object->payload;
		object = framebuffer->view;
		if (object == NULL || object->payload == NULL)
			return false;
	}
	if (object->kind == I915_VK_OBJ_IMAGE_VIEW) {
		view = object->payload;
		object = view->owner.parent;
		if (object == NULL || object->payload == NULL)
			return false;
	}
	if (object->kind != I915_VK_OBJ_IMAGE && object->kind != I915_VK_OBJ_BUFFER)
		return false;

	/* The bound memory's run. */
	resource = object->payload;
	if (resource->memory == NULL || resource->memory->payload == NULL)
		return false;
	memory = resource->memory->payload;
	if (memory->view == NULL || memory->view->buffer == NULL)
		return false;
	buffer = memory->view->buffer;
	enabled = spin_lock_irqsave(&buffer->guard);

	displayed = buffer->displayed != 0;

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Succeeded: reports whether the display holds the run. */
	return displayed;
}

/* Publishes the submission's reserved binary operations and its pending fence. */
static void
publish_reservations(
	struct queue_owner *owner)
{
	struct bcm2711_vulkan_sync *sync;
	uint32_t index;

	/* Each distinct binary payload counts one more execution with operations on it. */
	for (index = 0; index < owner->sync_count; index++) {
		sync = owner->syncs[index].object->payload;
		sync->signals_reserved = owner->syncs[index].signals;
		sync->waits_reserved = owner->syncs[index].waits;
		sync->pending++;
	}

	/* The fence cannot be reset or observed completed before the execution ends. */
	if (owner->fence != NULL) {
		sync = owner->fence->payload;
		sync->pending++;
	}

	/* Succeeded: later submissions and fence operations see the reservation. */
	return;
}

/*
 * Runs one step of an execution: the binary waits and signals up to the next
 * native pass, and that pass.  A command buffer's job runs one pass per step;
 * the execution is done after its last action.
 */
static int
execution_step(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_execution *base,
	bool *done)
{
	struct queue_execution *execution;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_sync *sync;
	struct queue_action *action;
	uint64_t entered;
	bool finished;
	bool retired;
	bool ran;
	int disposed;
	int error;

	/* Actions run in submission order from where the last step stopped. */
	execution = (struct queue_execution *)base;
	*done = false;
	ran = false;
	while (execution->cursor < execution->owner.action_count) {
		action = &execution->owner.actions[execution->cursor];

		/* Binary counters advance at their ordered point; earlier executions of the session already ran. */
		if (action->kind != QUEUE_COMMAND) {
			sync = action->object->payload;
			if (action->kind == QUEUE_WAIT) {
				sync->waits_completed++;
				sync->signaled = false;
			} else {
				sync->signals_completed++;
				sync->signaled = true;
			}
			execution->cursor++;
			continue;
		}

		/* One native pass per step: a second command waits for the next step. */
		if (ran)
			return 0;
		ran = true;

		/* Runs the job's next pass; the step's own time is the worker's share in the cost report. */
		entered = clock_ticks();
		error = bcm2711_vulkan_native_job_step(controller, execution->session->render, action->job, &finished, &retired);
		bcm2711_vulkan_native_job_count_step(controller, clock_ticks() - entered);
		command = action->object->payload;
		if (action->job->executed && (command->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0)
			command->state = BCM2711_VULKAN_COMMAND_INVALID;
		if (error != 0) {
			/* A pass that failed while preparing launched nothing, so only this execution fails. */
			if (action->job->prepare_failed && retired)
				base->engine = false;
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "submit failed error=%d", error);
			return error;
		}

		/* A nominal runner success without actual DMA retirement can never signal this submission's fence; the refusal says why. */
		if (!retired) {
			bcm2711_render_refusal_line("queue execution", "a job step ended without proof its native work retired");
			return EIO;
		}
		if (!finished)
			return 0;
#if CONFIG_DRIVER_BCM2711_VIDEO
		/* The command buffer's decodes were sent; its fence waits for those still in flight. */
		if (execution->owner.fence != NULL)
			bcm2711_vulkan_video_mark_commands(&execution->mark, action->object->payload);
#endif
		disposed = bcm2711_vulkan_native_job_dispose(controller, action->job, retired);
		action->job = NULL;
		if (disposed != 0)
			return disposed;
		execution->cursor++;
	}

#if CONFIG_DRIVER_BCM2711_VIDEO
	/*
	 * An execution with a fence ends only when the frames of its decodes are
	 * in their images: the host may read them once the fence signals.  It
	 * leaves the queue now and settles meanwhile, so the session's next
	 * decodes are sent behind it.
	 */
	if (execution->owner.fence != NULL)
		execution_mark(execution);
#endif

	/* Succeeded: every action ran. */
	*done = true;
	return 0;
}

/* Publishes the execution's completion or loss, releases what it owns and frees it. */
static int
execution_end(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_execution *base,
	int error)
{
	struct queue_execution *execution;
	int released;

	/* The payloads see the outcome before the session's later markers complete. */
	(void)controller;
	execution = (struct queue_execution *)base;
	if (error == 0) {
		publish_completion(&execution->owner);
	} else {
		publish_failure(&execution->owner);
	}

	/* Whole uncertain primary jobs transfer to controller quarantine before the payload edges retire. */
	released = release_owner(execution->session, &execution->owner);
	kern_free(execution);
	if (released != 0)
		return released;

	/* Succeeded: the execution and everything it owned retired. */
	return 0;
}

#if CONFIG_DRIVER_BCM2711_VIDEO
/*
 * Gives an execution with a fence its settle when decodes it waits for are
 * still in flight.  An empty submission (no command buffer) waits for every
 * decode in flight, as vkQueueWaitIdle asks; the command buffers' decodes
 * were added to the mark as their jobs ended.
 */
static void
execution_mark(
	struct queue_execution *execution)
{
	uint32_t index;
	bool commands;

	/* Whether the submission has a command buffer. */
	commands = false;
	for (index = 0; index < execution->owner.action_count; index++) {
		if (execution->owner.actions[index].kind == QUEUE_COMMAND)
			commands = true;
	}

	/* A submission that only waits for the queue covers every decode in flight. */
	if (!commands)
		bcm2711_vulkan_video_mark_all(&execution->mark);

	/* Decodes in flight to wait for give the execution its settle. */
	if (execution->mark.generation != 0)
		execution->base.settle = execution_settle;

	/* Succeeded: the execution settles, or ends at once. */
	return;
}

/* Tells the executor whether the frames an execution's fence waits for are in their images. */
static int
execution_settle(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_execution *base,
	bool wait)
{
	struct queue_execution *execution;
	int error;

	/* The mark's decodes, looked for a little when the executor may wait. */
	execution = (struct queue_execution *)base;
	error = bcm2711_vulkan_video_settle_mark(controller, &execution->mark, wait);
	if (error != 0)
		return error;

	/* Succeeded: the execution may end and signal its fence. */
	return 0;
}
#endif

/* Ends one execution's operations on its payloads and signals its fence. */
static void
publish_completion(
	struct queue_owner *owner)
{
	struct bcm2711_vulkan_sync *sync;
	uint32_t index;

	/* Each binary payload has one execution fewer with pending operations. */
	for (index = 0; index < owner->sync_count; index++) {
		sync = owner->syncs[index].object->payload;
		sync->pending--;
	}

	/* Only full successful native and metadata retirement signals the optional fence, including an empty submit. */
	if (owner->fence != NULL) {
		sync = owner->fence->payload;
		sync->pending--;
		sync->signaled = true;
	}

	/* Succeeded: the fence and the payloads show the completion. */
	return;
}

/* Releases unlaunched and retired jobs, transfers uncertain ones to quarantine, and drops the payload, fence and queue edges. */
static int
release_owner(
	struct bcm2711_vulkan_session *session,
	struct queue_owner *owner)
{
	struct queue_action *action;
	uint32_t index;
	int first;
	int error;

	/* Unlaunched and confirmed retired jobs release normally; an uncertain DMA root transfers without allocation to controller quarantine. */
	first = 0;
	for (index = 0; index < owner->action_count; index++) {
		action = &owner->actions[index];
		if (action->job == NULL)
			continue;
		error = bcm2711_vulkan_native_job_dispose(session->render->device, action->job, action->job->retired);
		action->job = NULL;
		if (first == 0 && error != 0)
			first = error;
	}

	/* Distinct binary CPU payloads retire after all native roots independently preserved their pending primary and session owners. */
	for (index = 0; index < owner->sync_count; index++) {
		error = bcm2711_vulkan_object_release(owner->syncs[index].object);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Optional fence and queue edges retire last while the complete CPU root remains the unwind authority. */
	error = bcm2711_vulkan_object_release(owner->fence);
	if (first == 0 && error != 0)
		first = error;
	error = bcm2711_vulkan_object_release(owner->queue);
	if (first == 0 && error != 0)
		first = error;
	if (first != 0)
		return first;

	/* Succeeded: no CPU prefix lost an independent native owner, logical parent or quarantine root. */
	return 0;
}

/*
 * Ends one failed or canceled execution's operations with sticky loss.
 *
 * Only this execution's payloads and fence fail; the executor decides
 * whether the failure is the engine's.
 */
static void
publish_failure(
	struct queue_owner *owner)
{
	struct bcm2711_vulkan_sync *sync;
	uint32_t index;

	/* Each binary payload is lost, with one execution fewer pending on it. */
	for (index = 0; index < owner->sync_count; index++) {
		sync = owner->syncs[index].object->payload;
		sync->status = VK_ERROR_DEVICE_LOST;
		sync->pending--;
	}

	/* A failed fence remains unsignaled with sticky loss even when preceding actions already modified images or consumed waits. */
	if (owner->fence != NULL) {
		sync = owner->fence->payload;
		sync->status = VK_ERROR_DEVICE_LOST;
		sync->pending--;
		sync->signaled = false;
	}

	/* Succeeded: the loss is visible before the session's later markers complete. */
	return;
}
