/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Logical callbacks, whole pending graph ownership and checked native DMA retirement are separate boundaries. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
#include "drivers/gpu/videocore/vk/vulkan-video.h"
#endif
#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/vk/vulkan-native-job.h"
#include "drivers/gpu/videocore/vk/vulkan-barrier.h"
#include "drivers/gpu/videocore/vk/vulkan-native-image.h"
#include "drivers/gpu/videocore/vk/vulkan-buffer-copy.h"
#include "drivers/gpu/videocore/vk/vulkan-memory.h"

/* All concurrently resident native pass/draw uploads share this submission-wide padded budget. */
#define NATIVE_JOB_BYTES (256ULL * 1024U * 1024U)

static int release_job(struct bcm2711_vulkan_native_job *job);
static int retire_closed_sessions(struct bcm2711_render_device *controller);

/*
 * Prepares an independently pending primary graph before any native allocation or worker acceptance.
 *
 * The renderer owns its exact Vulkan namespace, so uncertain work can keep
 * that session alive after common close.  Submit-time preparation copies
 * descriptor identity/state only; coherent UBO, vertex and texture bytes are
 * read later at FIFO execution after preceding native writes retire.
 */
int
bcm2711_vulkan_native_job_create(
	struct bcm2711_vulkan_object *command,
	struct bcm2711_vulkan_native_job **job)
{
	struct bcm2711_vulkan_native_job *created;
	struct bcm2711_render_session *session;
	int error;

	/* No partial pending graph escapes an ordinary allocation refusal. */
	if (job == NULL)
		return EINVAL;
	*job = NULL;

	/* A typed primary requires an exact renderer-owned non-closing Vulkan session. */
	if (command == NULL ||
	    command->session == NULL ||
	    command->kind != I915_VK_OBJ_COMMAND_BUFFER)
		return EINVAL;
	session = command->session->render;
	if (session == NULL ||
	    session->device == NULL ||
	    session->vulkan != command->session ||
	    command->session->closing)
		return EINVAL;

	/* The CPU payload root exists before the primary graph acquires its pending counts and cloned descriptor holds. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->session = session;
	created->retired = true;
	error = bcm2711_vulkan_prepared_create(command, &created->prepared);
	if (error != 0) {
		kern_free(created);
		return error;
	}

	/* Succeeded: worker acceptance or no-launch disposal owns this complete independently pending primary payload. */
	*job = created;
	return 0;
}

/*
 * Runs one step of a job: the CPU-only events up to the next native pass, that pass, and its release.
 *
 * The first step starts the job; the step that reaches the end of the
 * prepared events reports done.
 */
int
bcm2711_vulkan_native_job_step(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	void *payload,
	bool *done,
	bool *retired)
{
	struct bcm2711_vulkan_native_job *job;
	const struct bcm2711_vulkan_prepared_event *event;
	const struct bcm2711_vulkan_prepared_event *next;
	struct bcm2711_v3d_job_result completion;
	uint64_t remaining;
	uint64_t bytes;
	uint32_t region;
	int tiled;
	int error;
	int closed;
	int synchronized;

	/* Initial refusals launch no work and therefore retain no device borrow. */
	if (retired == NULL || done == NULL)
		return EINVAL;
	*retired = true;
	*done = false;
	job = payload;

	/* The accepted payload must belong to the same exact renderer namespace and native controller. */
	if (controller == NULL ||
	    session == NULL ||
	    job == NULL ||
	    job->prepared == NULL ||
	    job->session != session ||
	    session->device != controller ||
	    controller->space.native == NULL)
		return EINVAL;

	/* Only this step's failure can be preparation's. */
	job->prepare_failed = false;

	/* A completed or uncertain payload never replays coherent source reads, clear or native work. */
	if (job->quarantined || (job->executed && !job->stepping)) {
		*retired = job->retired;
		return EBUSY;
	}

	/* A stopped controller refuses both native launches and CPU layout publication before the primary becomes single-use work. */
	if (!controller->space.native->hardware.initialized ||
	    !controller->space.native->hardware.ready ||
	    !controller->space.native->power.ready)
		return EIO;

	/* The first step starts the job: one finite resident upload budget covers every pass and its complete independent draw input prefix. */
	if (!job->executed) {
		job->executed = true;
		job->stepping = true;
		job->remaining = NATIVE_JOB_BYTES;
		job->region = 0;
		job->cursor = job->prepared->first;
	}

	/* This step continues where the last one stopped. */
	remaining = job->remaining;
	region = job->region;
	event = job->cursor;
	while (event != NULL) {
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
		/*
		 * A copy, blit or upload that names an image whose decode is in
		 * flight waits for its frame first, a step at a time: the step
		 * ends at the command, the other sessions take their turns, and
		 * the next step comes back here.
		 */
		if (event->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE ||
		    event->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER ||
		    event->opcode == GPU_OP_CMD_COPY_IMAGE ||
		    event->opcode == GPU_OP_CMD_BLIT_IMAGE) {
			error = bcm2711_vulkan_video_settle(controller, event->record);
			if (error == EINPROGRESS) {
				job->cursor = event;
				job->region = region;
				job->remaining = remaining;
				return 0;
			}

			/* A wait that failed fails the job before the command runs. */
			if (error != 0) {
				job->prepare_failed = true;
				return error;
			}
		}
#endif

		/* A whole-image upload or readback becomes a tile copy pass; ENOTSUP leaves it to the CPU. */
		tiled = ENOTSUP;
		if (event->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || event->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
			error = bcm2711_v3d_memory_begin(&controller->space);
			if (error != 0)
				return error;
			tiled = bcm2711_vulkan_native_buffer_image_create(&controller->space, event->record, &remaining, &job->pass);
			synchronized = bcm2711_v3d_memory_end(&controller->space);
			if (tiled == 0 && synchronized != 0)
				tiled = synchronized;

			/* A refused tile copy with its edits published failed only in preparation. */
			if (tiled != 0 && tiled != ENOTSUP) {
				if (synchronized == 0)
					job->prepare_failed = true;
				return tiled;
			}
		}

		/* Exact coherent buffer transfers execute only after every preceding native pass has retired and published its output. */
		if (tiled == ENOTSUP &&
		    (event->opcode == GPU_OP_CMD_COPY_BUFFER ||
		     event->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE ||
		     event->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)) {
			error = bcm2711_vulkan_buffer_copy_run(event->record);
			if (error != 0) {
				job->prepare_failed = true;
				return error;
			}
			event = event->next;
			continue;
		}

		/* Every earlier native pass completed before this explicit dependency can publish FIFO-visible layout state. */
		if (event->opcode == GPU_OP_CMD_PIPELINE_BARRIER) {
			error = bcm2711_vulkan_barrier_run(event->record);
			if (error != 0) {
				job->prepare_failed = true;
				return error;
			}
			event = event->next;
			continue;
		}

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
		/* Video and status query events run on the CPU, in order, after every earlier pass. */
		if (bcm2711_vulkan_video_event(event->opcode)) {
			error = bcm2711_vulkan_video_run(controller, event->record);

			/*
			 * A decode whose picture the firmware has not given back
			 * yet ends the step at itself: the other sessions take
			 * their turns, and the next step runs it again to look for
			 * the picture.
			 */
			if (error == EINPROGRESS) {
				job->cursor = event;
				job->region = region;
				job->remaining = remaining;
				return 0;
			}

			/* A video event that failed fails the job before anything after it runs. */
			if (error != 0) {
				job->prepare_failed = true;
				return error;
			}
			event = event->next;
			continue;
		}
#endif

		/* Defers input translations until one checked publication. */
		error = bcm2711_v3d_memory_begin(&controller->space);
		if (error != 0)
			return error;

		/*
		 * Graphics, clears and copied image regions acquire complete
		 * independent native owners; their owned storage carries this
		 * session's mark only while they are built.
		 */
		controller->space.builder = session->owner;
		if (tiled == 0) {
			error = 0;
			next = event->next;
		} else if (event->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
			error = bcm2711_vulkan_native_pass_create(&controller->space, event, &remaining, &job->pass, &next);
		} else if (event->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
			error = bcm2711_vulkan_native_clear_create(&controller->space, event, &remaining, &job->pass);
			next = event->next;
		} else if (event->opcode == GPU_OP_CMD_COPY_IMAGE || event->opcode == GPU_OP_CMD_BLIT_IMAGE) {
			error = bcm2711_vulkan_native_image_create(&controller->space, (const struct bcm2711_vulkan_transfer *)event->record, region, &remaining, &job->pass);
			region++;
			next = event;
			if (region == event->record->count) {
				next = event->next;
				region = 0;
			}
		} else {
			error = ENOTSUP;
		}
		controller->space.builder = 0;

		/* Even a rolled-back preparation must flush its edits before any retired VA or RAM is reused. */
		synchronized = bcm2711_v3d_memory_end(&controller->space);
		if (synchronized != 0)
			error = synchronized;

		/* A refusal whose edits were flushed failed only in preparation. */
		if (error != 0 && synchronized == 0)
			job->prepare_failed = true;

		/* A copy whose destination already holds its source's bytes prepared no pass. */
		if (error == 0 && job->pass == NULL) {
			event = next;
			continue;
		}

		/* A refused whole-pass preparation owns no launched DMA and leaves the primary pending until disposal. */
		if (error != 0) {
			kern_logf("v3d: submit prepare opcode=%u failed (%d)\n", event->opcode, error);
			return error;
		}

		/*
		 * Exact clear, supervised bin/render and GPU cache visibility execute
		 * while every independent owner is held.  The native jobs run inside
		 * the window, without the controller mutex: the decoder and ioctls
		 * proceed meanwhile, and no translation changes.
		 */
		error = bcm2711_vulkan_native_pass_start(job->pass, &completion);
		if (error != 0) {
			/* Nothing launched: the pass failed only in preparation. */
			job->prepare_failed = true;
		} else {
			bcm2711_render_window_open(controller);
			error = bcm2711_vulkan_native_pass_launch(job->pass, &completion);
			closed = bcm2711_render_window_close(controller);
			error = bcm2711_vulkan_native_pass_finish(job->pass, &completion, error);
			if (error == 0 && closed != 0)
				error = closed;
		}
		job->retired = completion.retired;
		*retired = completion.retired;
		if (error != 0) {
			kern_logf("v3d: submit execute opcode=%u draws=%u size=%ux%u failed (%d) retired=%u\n",
				  event->opcode,
				  job->pass->draws,
				  job->pass->state.width,
				  job->pass->state.height,
				  error,
				  (unsigned)completion.retired);
			return error;
		}

		/* Confirmed DMA retirement permits this resident pass budget and its mappings to be reused by the next FIFO pass. */
		bytes = job->pass->bytes;
		error = bcm2711_v3d_memory_begin(&controller->space);
		if (error != 0)
			return error;

		/* Consumes all stopped pass roots while their physical allocations remain batch-owned. */
		error = bcm2711_vulkan_native_pass_release(&job->pass, true);

		/* Physical backing stays owned until the complete batch of PTE withdrawals is flushed. */
		synchronized = bcm2711_v3d_memory_end(&controller->space);
		if (synchronized != 0)
			error = synchronized;
		if (error != 0)
			return error;
		remaining += bytes;

		/* One pass ends the step; the next step starts at the following event. */
		job->cursor = next;
		job->region = region;
		job->remaining = remaining;
		if (next != NULL)
			return 0;
		event = next;
	}

	/* The last step: the job ran every pass. */
	job->cursor = NULL;
	job->stepping = false;
	*done = true;

	/* Succeeded: the pending primary remains owned until worker disposal, after actual native completion of every pass. */
	return 0;
}

/*
 * Disposes a completed payload or transfers a whole uncertain payload to persistent controller ownership.
 *
 * Worker callback retirement always follows this operation.  False retirement
 * therefore keeps the pending primary, all descriptor charges, current pass
 * mappings, output and session alive in the controller list with no allocation.
 */
int
bcm2711_vulkan_native_job_dispose(
	struct bcm2711_render_device *controller,
	void *payload,
	bool retired)
{
	struct bcm2711_vulkan_native_job *job;
	int error;

	/* Only one exact renderer/controller can consume or persist this prepared root. */
	job = payload;
	if (controller == NULL ||
	    job == NULL ||
	    job->session == NULL ||
	    job->session->device != controller)
		return EINVAL;

	/* Uncertain native DMA transfers the existing whole root without a fallible allocation or loss of its pending owner graph. */
	if (!retired) {
		/* A repeated false disposal must not duplicate the same root in the persistent list. */
		if (job->quarantined)
			return 0;
		job->quarantined = true;
		job->retired = false;
		job->next = controller->quarantine;
		controller->quarantine = job;
		return 0;
	}

	/* A controller-owned quarantine entry may retire only through the explicit global-reset recovery traversal. */
	if (job->quarantined)
		return EBUSY;

	/* Whole native owners retire before the pending primary releases descriptor charges and typed dependencies. */
	error = release_job(job);
	if (error != 0)
		return error;

	/* Succeeded: no worker callback can lose an uncertain whole payload or its session lifetime. */
	return 0;
}

/*
 * Retires all persistent native payloads and closed sessions after checked global reset stops DMA.
 *
 * External sessions must already have closed.  Logical owner retirement is
 * completed even after a mapping teardown refusal; the address space keeps
 * failed final translations/physical allocations for the next checked reset.
 */
int
bcm2711_vulkan_native_jobs_recover(
	struct bcm2711_render_device *controller)
{
	struct bcm2711_vulkan_native_job *job;
	int first;
	int error;

	/* Missing native recovery ownership supplies no valid DMA-stop boundary. */
	if (controller == NULL || controller->space.native == NULL)
		return EINVAL;

	/* Surviving external sessions or failed native reset leave every persistent whole root untouched. */
	if (controller->sessions != 0)
		return EBUSY;
	if (!controller->space.native->hardware.ready)
		return EIO;

	/* Checked reset permits every CPU root to retire while failed translations remain independently space-owned. */
	first = 0;
	while (controller->quarantine != NULL) {
		job = controller->quarantine;
		controller->quarantine = job->next;
		job->next = NULL;
		job->quarantined = false;
		error = release_job(job);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Closed Vulkan namespaces retain their renderer descriptors until all former prepared destructors stop borrowing them. */
	error = retire_closed_sessions(controller);
	if (first == 0 && error != 0)
		first = error;
	if (first != 0)
		return first;

	/* Succeeded: no logical native payload or closed session remains hidden behind callback completion. */
	return 0;
}

/* Consumes the complete native pass before dropping pending primary/descriptor references and finally the CPU payload root. */
static int
release_job(
	struct bcm2711_vulkan_native_job *job)
{
	int first;
	int error;

	/* Checked DMA retirement consumes all mapped inputs, including incomplete allocation prefixes. */
	first = bcm2711_vulkan_native_pass_release(&job->pass, true);

	/* Pending counts and cloned descriptors remain valid through the last native pass destructor. */
	error = bcm2711_vulkan_prepared_release(job->prepared, true);
	if (first == 0 && error != 0)
		first = error;
	kern_free(job);
	if (first != 0)
		return first;

	/* Succeeded: every independently pending primary and native pass input has retired once. */
	return 0;
}

/* Releases closed renderer descriptors only after their final Vulkan dependency graph and protocol arena can retire. */
static int
retire_closed_sessions(
	struct bcm2711_render_device *controller)
{
	struct bcm2711_render_session **position;
	struct bcm2711_render_session *session;
	int first;
	int error;

	/* A failed logical close stays linked for the next explicit recovery, with its renderer pointer still valid. */
	first = 0;
	position = &controller->closed;
	while (*position != NULL) {
		session = *position;
		error = bcm2711_vulkan_session_close(&session->vulkan);
		if (first == 0 && error != 0)
			first = error;

		/* Remaining typed owners need this same renderer descriptor for their eventual destructors. */
		if (session->vulkan != NULL) {
			position = &session->closed_next;
			continue;
		}

		/* The entire protocol graph has retired, so the closed renderer has no remaining borrower. */
		*position = session->closed_next;
		kern_free(session);
	}

	/* A retirement refusal stays visible even when complete logical prefixes were safely consumed. */
	if (first != 0)
		return first;

	/* Succeeded: every closed namespace and its internally retained renderer descriptor has retired. */
	return 0;
}
