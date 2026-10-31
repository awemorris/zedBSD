/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Logical callbacks, whole pending graph ownership and checked native DMA retirement are separate boundaries. */
#include <stdarg.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_BCM2711_VIDEO
#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#endif
#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/vk/vulkan-native-job.h"
#include "drivers/gpu/bcm2711/vk/vulkan-barrier.h"
#include "drivers/gpu/bcm2711/vk/vulkan-native-image.h"
#include "drivers/gpu/bcm2711/vk/vulkan-buffer-copy.h"
#include "drivers/gpu/bcm2711/vk/vulkan-memory.h"

/* All concurrently resident native pass/draw uploads share this submission-wide padded budget. */
#define NATIVE_JOB_BYTES (256ULL * 1024U * 1024U)

/* The room of one window's cost report, formatted under the controller mutex and logged after it. */
#define NATIVE_PERF_TEXT_BYTES 2048U

/* A pass this slow (prepared and run, in milliseconds) gets its own line, a few a window. */
#define NATIVE_SLOW_MS 20U
#define NATIVE_SLOW_LINES 4U

/* Whether the boot asked for the cost report (v3d.perf=1): not read yet, on, or off. */
#define NATIVE_PERF_UNREAD 0
#define NATIVE_PERF_ON 1
#define NATIVE_PERF_OFF 2

/*
 * The cost report a step formatted and the executor has yet to log
 * (bcm2711_vulkan_native_perf_flush), and whether the report is on.  The
 * executor's thread alone reads and writes them.
 */
static char native_perf_text[NATIVE_PERF_TEXT_BYTES];
static size_t native_perf_used;
static int native_perf_state;

/*
 * Whether the text holds a window's report (the board's line follows it),
 * and how many slow-pass lines the window has given (at most
 * NATIVE_SLOW_LINES, so that a scene slow in every frame does not flood
 * the log).
 */
static bool native_perf_window;
static uint32_t native_slow_lines;

static int release_job(struct bcm2711_vulkan_native_job *job);
static int retire_closed_sessions(struct bcm2711_render_device *controller);
static void report_perf(struct bcm2711_v3d_perf *perf);
static void slow_pass(const struct bcm2711_vulkan_native_pass *pass, uint32_t opcode, uint64_t prepare, uint64_t execute, const struct bcm2711_v3d_job_result *completion);
static bool perf_enabled(void);
static void perf_append(const char *format, ...) __attribute__((format(printf, 1, 2)));
static uint64_t average_us(uint64_t ticks, uint64_t count);

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
 * prepared events adds the job to the cost report and reports done.
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
	uint64_t started;
	uint64_t step;
	uint64_t end_step;
	uint64_t prepare_ticks;
	uint32_t tiles;
	uint64_t execute_ticks;
	uint64_t release_ticks;
	uint64_t copy_ticks;
	uint64_t elapsed;
	uint64_t pass_prepare;
	struct bcm2711_v3d_perf *perf;
	uint32_t region;
	uint32_t index;
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
		job->started = clock_ticks();
		job->prepare_ticks = 0;
		job->execute_ticks = 0;
		job->release_ticks = 0;
		job->copy_ticks = 0;
		job->remaining = NATIVE_JOB_BYTES;
		job->region = 0;
		job->cursor = job->prepared->first;
	}

	/* This step continues where the last one stopped. */
	perf = &controller->space.native->hardware.perf;
	started = job->started;
	prepare_ticks = job->prepare_ticks;
	execute_ticks = job->execute_ticks;
	release_ticks = job->release_ticks;
	copy_ticks = job->copy_ticks;
	remaining = job->remaining;
	region = job->region;
	event = job->cursor;
	while (event != NULL) {
#if CONFIG_DRIVER_BCM2711_VIDEO
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
			step = clock_ticks();
			error = bcm2711_vulkan_video_settle(controller, event->record);
			copy_ticks += clock_ticks() - step;
			if (error == EINPROGRESS) {
				job->cursor = event;
				job->region = region;
				job->remaining = remaining;
				job->prepare_ticks = prepare_ticks;
				job->execute_ticks = execute_ticks;
				job->release_ticks = release_ticks;
				job->copy_ticks = copy_ticks;
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
			step = clock_ticks();
			error = bcm2711_v3d_memory_begin(&controller->space);
			if (error != 0)
				return error;
			tiled = bcm2711_vulkan_native_buffer_image_create(&controller->space, event->record, &remaining, &job->pass);
			synchronized = bcm2711_v3d_memory_end(&controller->space);
			prepare_ticks += clock_ticks() - step;
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
			step = clock_ticks();
			error = bcm2711_vulkan_buffer_copy_run(event->record);
			copy_ticks += clock_ticks() - step;
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

#if CONFIG_DRIVER_BCM2711_VIDEO
		/* Video and status query events run on the CPU, in order, after every earlier pass. */
		if (bcm2711_vulkan_video_event(event->opcode)) {
			step = clock_ticks();
			error = bcm2711_vulkan_video_run(controller, event->record);
			copy_ticks += clock_ticks() - step;

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
				job->prepare_ticks = prepare_ticks;
				job->execute_ticks = execute_ticks;
				job->release_ticks = release_ticks;
				job->copy_ticks = copy_ticks;
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

		/* Measures preparation while deferring input translations until one checked publication. */
		step = clock_ticks();
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

		/* Accumulate preparation separately from device execution and retirement. */
		pass_prepare = clock_ticks() - step;
		prepare_ticks += pass_prepare;

		/* A copy whose destination already holds its source's bytes prepared no pass. */
		if (error == 0 && job->pass == NULL) {
			perf->skipped_copies++;
			event = next;
			continue;
		}

		/* The cost report separates graphics passes, image clears and image transfers. */
		if (error == 0) {
			bcm2711_vulkan_native_pass_costs(job->pass, perf);
			tiles = bcm2711_native_pass_tiles(&job->pass->state);
			if (event->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
				perf->render_passes++;
				perf->render_tiles += tiles;
			} else if (event->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
				perf->clear_passes++;
			} else {
				perf->image_passes++;
				perf->image_tiles += tiles;
			}
		}

		/* A refused whole-pass preparation owns no launched DMA and leaves the primary pending until disposal. */
		if (error != 0) {
			bcm2711_stage_mark(
				BCM2711_FAMILY_V3D,
				"submit prepare opcode=%u error=%d",
				event->opcode,
				error);
			return error;
		}

		/*
		 * Exact clear, supervised bin/render and GPU cache visibility execute
		 * while every independent owner is held.  The native jobs run inside
		 * the window, without the controller mutex: the decoder and ioctls
		 * proceed meanwhile, and no translation changes.
		 */
		step = clock_ticks();
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
		execute_ticks += clock_ticks() - step;
		slow_pass(job->pass, event->opcode, pass_prepare, clock_ticks() - step, &completion);
		perf->tfu_ticks += completion.tfu_ticks;
		perf->tfu_jobs += completion.tfu_jobs;
		if (completion.tfu_max_ticks > perf->tfu_max_ticks)
			perf->tfu_max_ticks = completion.tfu_max_ticks;
		perf->bin_ticks += completion.bin_ticks;
		perf->render_ticks += completion.render_ticks;
		perf->output_clean_ticks += completion.clean_ticks;
		perf->counters_71 = completion.counters_71;
		if (completion.bin_counters[0] != 0U) {
			for (index = 0; index < BCM2711_V3D_COUNTERS; index++)
				perf->bin_counters[index] += completion.bin_counters[index];
			perf->bin_counter_runs++;
		}
		if (completion.counters[0] != 0U) {
			for (index = 0; index < BCM2711_V3D_COUNTERS; index++)
				perf->counters[index] += completion.counters[index];
			perf->counter_runs++;

			/* Tile copies and clears only load and store tiles; drawing passes also shade. */
			if (job->pass->draws == 0) {
				perf->copy_cycles += completion.counters[0];
				perf->copy_tiles += tiles;
			} else {
				perf->draw_cycles += completion.counters[0];
				perf->draw_tiles += tiles;
			}
		}
		job->retired = completion.retired;
		*retired = completion.retired;
		if (error != 0) {
			bcm2711_stage_mark(
				BCM2711_FAMILY_V3D,
				"submit execute opcode=%u draws=%u size=%ux%u error=%d retired=%u",
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
		step = clock_ticks();
		error = bcm2711_v3d_memory_begin(&controller->space);
		if (error != 0)
			return error;

		/* Consumes all stopped pass roots while their physical allocations remain batch-owned. */
		error = bcm2711_vulkan_native_pass_release(&job->pass, true);

		/* Physical backing stays owned until the complete batch of PTE withdrawals is flushed. */
		end_step = clock_ticks();
		synchronized = bcm2711_v3d_memory_end(&controller->space);
		perf->release_end_ticks += clock_ticks() - end_step;
		if (synchronized != 0)
			error = synchronized;
		release_ticks += clock_ticks() - step;
		if (error != 0)
			return error;
		remaining += bytes;

		/* One pass ends the step; the next step starts at the following event. */
		job->cursor = next;
		job->region = region;
		job->remaining = remaining;
		job->prepare_ticks = prepare_ticks;
		job->execute_ticks = execute_ticks;
		job->release_ticks = release_ticks;
		job->copy_ticks = copy_ticks;
		if (next != NULL)
			return 0;
		event = next;
	}

	/* The last step: the job ran every pass. */
	job->cursor = NULL;
	job->stepping = false;
	*done = true;

	/* Adds this completed submission to the periodic cost report. */
	elapsed = clock_ticks() - started;
	perf->submissions++;
	perf->total_ticks += elapsed;
	if (elapsed > perf->max_ticks)
		perf->max_ticks = elapsed;
	perf->prepare_ticks += prepare_ticks;
	perf->execute_ticks += execute_ticks;
	perf->release_ticks += release_ticks;
	perf->transfer_ticks += copy_ticks;
	report_perf(perf);

	/* Succeeded: the pending primary remains owned until worker disposal, after actual native completion of every pass. */
	return 0;
}

/*
 * Adds one job step's own time to the cost report's worker share.
 *
 * The executor measures each step around bcm2711_vulkan_native_job_step;
 * a controller without native hardware has no report.
 */
void
bcm2711_vulkan_native_job_count_step(
	struct bcm2711_render_device *controller,
	uint64_t ticks)
{
	/* No report without the native hardware. */
	if (controller == NULL || controller->space.native == NULL)
		return;

	/* The step held the worker for this long. */
	controller->space.native->hardware.perf.step_ticks += ticks;

	/* Succeeded: the window's worker share includes the step. */
	return;
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

/*
 * Prints one line of submission cost totals every two seconds and starts a new window.
 *
 * Averages are per submission in microseconds, so one line summarizes many
 * frames without adding console output to each submission.
 */
static void
report_perf(
	struct bcm2711_v3d_perf *perf)
{
	uint64_t now;
	uint64_t count;
	uint64_t busy;
	uint64_t worker;
	uint64_t released;
	uint64_t released_bytes;
	uint64_t released_ticks;
	uint64_t created[9];
	uint64_t in_use;

	/* The first submission opens the window that later reports measure. */
	now = clock_ticks();
	if (perf->window_start == 0) {
		perf->window_start = now;
		return;
	}

	/* A window shorter than two seconds keeps accumulating. */
	if (kern_ticks_to_ms(now - perf->window_start) < 2000U)
		return;

	/* The share of the window the submissions held the worker shows how much CPU and GPU time frames took. */
	count = perf->submissions;
	busy = perf->total_ticks * 100U / (now - perf->window_start);
	worker = perf->step_ticks * 100U / (now - perf->window_start);

	/* Prints the window's per-submission averages and its worst submission. */
	perf_append(
		"v3d: perf ms=%llu busy=%llu%% subs=%llu passes=%u/%u/%u skipped=%u draws=%u kib=%llu avg_us total=%llu prep=%llu tex=%llu copy=%llu prog=%llu rec=%llu alloc=%llu map=%llu clean=%llu exec=%llu tfu=%llu bin=%llu render=%llu oclean=%llu rel=%llu xfer=%llu max_us=%llu snap=%u/%u/%u/%u upd=%u mapped_kib=%llu largest=%ux%u%c tiles=%u/%u worker=%llu%%\n",
		(unsigned long long)kern_ticks_to_ms(now - perf->window_start),
		(unsigned long long)busy,
		(unsigned long long)count,
		perf->render_passes,
		perf->clear_passes,
		perf->image_passes,
		perf->skipped_copies,
		perf->draws,
		(unsigned long long)(perf->staged_bytes / 1024U),
		(unsigned long long)average_us(perf->total_ticks, count),
		(unsigned long long)average_us(perf->prepare_ticks, count),
		(unsigned long long)average_us(perf->texture_ticks, count),
		(unsigned long long)average_us(perf->texel_copy_ticks, count),
		(unsigned long long)average_us(perf->program_ticks, count),
		(unsigned long long)average_us(perf->record_ticks, count),
		(unsigned long long)average_us(perf->allocation_ticks, count),
		(unsigned long long)average_us(perf->mapping_ticks, count),
		(unsigned long long)average_us(perf->clean_ticks, count),
		(unsigned long long)average_us(perf->execute_ticks, count),
		(unsigned long long)average_us(perf->tfu_ticks, count),
		(unsigned long long)average_us(perf->bin_ticks, count),
		(unsigned long long)average_us(perf->render_ticks, count),
		(unsigned long long)average_us(perf->output_clean_ticks, count),
		(unsigned long long)average_us(perf->release_ticks, count),
		(unsigned long long)average_us(perf->transfer_ticks, count),
		(unsigned long long)average_us(perf->max_ticks, 1U),
		perf->snapshot_hits,
		perf->snapshot_fills,
		perf->mapped_textures,
		perf->private_textures,
		perf->snapshot_updates,
		(unsigned long long)(perf->mapped_bytes / 1024U),
		perf->largest_size >> 16,
		perf->largest_size & 0xffffU,
		perf->largest_kind != 0 ? perf->largest_kind : '-',
		perf->render_tiles,
		perf->image_tiles,
		(unsigned long long)worker);

	/* The render runs' performance counters, in thousands per run. */
	if (perf->counter_runs != 0U) {
		perf_append(
			perf->counters_71 ?
			"v3d: gpu runs=%u k/run cycles=%llu cle=%llu qactive=%llu qfrag=%llu qstall=%llu qfragwait=%llu stmu=%llu stlb=%llu sother=%llu tmu=%llu tmustall=%llu texquads=%llu l2miss=%llu quads=%llu written=%llu prims=%llu\n" :
			"v3d: gpu runs=%u k/run cycles=%llu qidle=%llu qfrag=%llu qvalid=%llu qtmu=%llu qsb=%llu qvary=%llu qstall=%llu tmu=%llu tmumiss=%llu tmustall=%llu cle=%llu quads=%llu written=%llu tlbrd=%llu tlbwr=%llu\n",
			perf->counter_runs,
			(unsigned long long)(perf->counters[0] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[1] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[2] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[3] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[4] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[5] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[6] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[7] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[8] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[9] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[10] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[11] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[12] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[13] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[14] / perf->counter_runs / 1000U),
			(unsigned long long)(perf->counters[15] / perf->counter_runs / 1000U));
	}

	/* The bin runs' counters, in thousands per run (the bin sources of run_cl in v3d-job.c). */
	if (perf->bin_counter_runs != 0U) {
		perf_append(
			perf->counters_71 ?
			"v3d: gpu bin runs=%u k/run cycles=%llu core=%llu cle=%llu qactive=%llu qstall=%llu svpm=%llu suni=%llu sother=%llu bubble=%llu ihit=%llu imiss=%llu vcdhit=%llu vcdmiss=%llu binned=%llu reversed=%llu outside=%llu\n" :
			"v3d: gpu bin runs=%u k/run cycles=%llu qidle=%llu qcoord=%llu qvalid=%llu qwait=%llu ihit=%llu imiss=%llu uhit=%llu umiss=%llu vdw=%llu vcd=%llu cle=%llu l2hit=%llu l2miss=%llu binned=%llu reversed=%llu\n",
			perf->bin_counter_runs,
			(unsigned long long)(perf->bin_counters[0] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[1] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[2] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[3] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[4] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[5] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[6] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[7] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[8] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[9] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[10] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[11] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[12] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[13] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[14] / perf->bin_counter_runs / 1000U),
			(unsigned long long)(perf->bin_counters[15] / perf->bin_counter_runs / 1000U));
	}

	/* Worker requests' FIFO waits and worker time, and the final buffer releases, in the window. */
	bcm2711_buffer_release_stats(&released, &released_bytes, &released_ticks);
	if (perf->requests != 0U) {
		perf_append(
			"v3d: requests=%llu wait_us avg=%llu max=%llu run_us avg=%llu max=%llu freed=%llu kib=%llu free_us=%llu markers_max=%u submit_lock_us avg=%llu max=%llu hold_us decode=%llu/cmds%u/op%u command=%llu/op%u step=%llu\n",
			(unsigned long long)perf->requests,
			(unsigned long long)(kern_ticks_to_rate(perf->request_wait_ticks, 1000000U) / perf->requests),
			(unsigned long long)kern_ticks_to_rate(perf->request_wait_max, 1000000U),
			(unsigned long long)(kern_ticks_to_rate(perf->request_run_ticks, 1000000U) / perf->requests),
			(unsigned long long)kern_ticks_to_rate(perf->request_run_max, 1000000U),
			(unsigned long long)released,
			(unsigned long long)(released_bytes / 1024U),
			(unsigned long long)kern_ticks_to_rate(released_ticks, 1000000U),
			perf->markers_max,
			(unsigned long long)(perf->submit_locks != 0U ? kern_ticks_to_rate(perf->submit_lock_ticks, 1000000U) / perf->submit_locks : 0U),
			(unsigned long long)kern_ticks_to_rate(perf->submit_lock_max, 1000000U),
			(unsigned long long)kern_ticks_to_rate(perf->decode_hold_max, 1000000U),
			perf->decode_hold_commands,
			perf->decode_hold_opcode,
			(unsigned long long)kern_ticks_to_rate(perf->decode_command_max, 1000000U),
			perf->decode_command_opcode,
			(unsigned long long)kern_ticks_to_rate(perf->step_hold_max, 1000000U));
	}

	/*
	 * The decoder on the fine counter: the average cost of one
	 * command, the longest request and its commands, and the most
	 * identities a session held (the tick-based command= of the requests
	 * line names the longest single command).
	 */
	if (perf->decode_requests != 0U &&
	    perf->decode_commands != 0U &&
	    perf->decode_frequency != 0U) {
		perf_append(
			"v3d: decode requests=%llu cmds=%llu us=%llu ns/cmd=%llu max_us=%llu/cmds%u objects=%u\n",
			(unsigned long long)perf->decode_requests,
			(unsigned long long)perf->decode_commands,
			(unsigned long long)(perf->decode_counts * 1000000U / perf->decode_frequency),
			(unsigned long long)(perf->decode_counts * 1000000000U / perf->decode_frequency / perf->decode_commands),
			(unsigned long long)(perf->decode_counts_max * 1000000U / perf->decode_frequency),
			perf->decode_counts_max_commands,
			perf->decode_objects_max);
	}

	/* Where the alloc time goes: the runs made (the allocator and their zeroing) and the reused runs zeroed again. */
	bcm2711_buffer_create_stats(created);
	if (created[0] != 0U || created[4] != 0U || created[7] != 0U) {
		perf_append(
			"v3d: alloc made=%llu kib=%llu pmem_us=%llu zero_us=%llu reused=%llu kib=%llu zero_us=%llu kept=%llu kib=%llu\n",
			(unsigned long long)created[0],
			(unsigned long long)(created[1] / 1024U),
			(unsigned long long)kern_ticks_to_rate(created[2], 1000000U),
			(unsigned long long)kern_ticks_to_rate(created[3], 1000000U),
			(unsigned long long)created[4],
			(unsigned long long)(created[5] / 1024U),
			(unsigned long long)kern_ticks_to_rate(created[6], 1000000U),
			(unsigned long long)created[7],
			(unsigned long long)(created[8] / 1024U));
	}

	/* Where the release time goes: draw teardown, the batch end's flush and frees, and the views' fate. */
	if (perf->submissions != 0U) {
		perf_append(
			"v3d: release draws=%llu draw_us=%llu end_us=%llu views cached=%llu unmapped=%llu\n",
			(unsigned long long)perf->released_draws,
			(unsigned long long)kern_ticks_to_rate(perf->release_draw_ticks, 1000000U),
			(unsigned long long)kern_ticks_to_rate(perf->release_end_ticks, 1000000U),
			(unsigned long long)perf->views_cached,
			(unsigned long long)perf->views_unmapped);
	}

	/* Cycles a tile takes when it is only loaded and stored, and when it is also drawn. */
	if (perf->copy_tiles != 0U || perf->draw_tiles != 0U) {
		perf_append(
			"v3d: gpu tiles copy=%u cycles/tile=%llu draw=%u cycles/tile=%llu\n",
			perf->copy_tiles,
			(unsigned long long)(perf->copy_tiles != 0U ? perf->copy_cycles / perf->copy_tiles : 0U),
			perf->draw_tiles,
			(unsigned long long)(perf->draw_tiles != 0U ? perf->draw_cycles / perf->draw_tiles : 0U));
	}

	/* The texture formatting unit's conversions: how many, and their average and longest wall time. */
	if (perf->tfu_jobs != 0U) {
		perf_append(
			"v3d: tfu jobs=%u avg_us=%llu max_us=%llu\n",
			perf->tfu_jobs,
			(unsigned long long)average_us(perf->tfu_ticks, perf->tfu_jobs),
			(unsigned long long)kern_ticks_to_rate(perf->tfu_max_ticks, 1000000U));
	}

	/* The Vulkan allocation budget: in use now, the window's most, and what it refused. */
	perf_append(
		"v3d: memory kib=%llu max_kib=%llu budget_kib=%llu refused=%u last_kib=%llu\n",
		(unsigned long long)(perf->memory_bytes / 1024U),
		(unsigned long long)(perf->memory_max / 1024U),
		(unsigned long long)(BCM2711_VULKAN_MEMORY_BUDGET / 1024U),
		perf->memory_refusals,
		(unsigned long long)(perf->memory_refused_bytes / 1024U));

	/* The next window starts empty at this report; the budget in use carries over. */
	in_use = perf->memory_bytes;
	kern_memset(perf, 0, sizeof(*perf));
	perf->window_start = now;
	native_perf_window = true;
	native_slow_lines = 0;
	perf->memory_bytes = in_use;
	perf->memory_max = in_use;
}

/* Tells whether the boot asked for the cost report, reading the boot line once. */
static bool
perf_enabled(
	void)
{
	/* The boot line is read at the first report. */
	if (native_perf_state == NATIVE_PERF_UNREAD) {
		native_perf_state = NATIVE_PERF_OFF;
		if (bcm2711_stage_perf_on())
			native_perf_state = NATIVE_PERF_ON;
	}

	/* On only when asked for. */
	return native_perf_state == NATIVE_PERF_ON;
}

/*
 * Reports a pass that took NATIVE_SLOW_MS or more to prepare and run (v3d.perf=1):
 * its preparation and execution, the texture work, program builds and
 * allocations of its draws, and its unit, bin and render times, in
 * microseconds.  A frame that misses the display's refresh is found by it.
 */
static void
slow_pass(
	const struct bcm2711_vulkan_native_pass *pass,
	uint32_t opcode,
	uint64_t prepare,
	uint64_t execute,
	const struct bcm2711_v3d_job_result *completion)
{
	const struct bcm2711_vulkan_native_draw *draw;
	uint64_t texture;
	uint64_t copy;
	uint64_t program;
	uint64_t allocation;
	uint32_t index;

	/* Only a slow pass, a few a window, and only with the report. */
	if (pass == NULL ||
	    kern_ticks_to_ms(prepare + execute) < NATIVE_SLOW_MS ||
	    native_slow_lines >= NATIVE_SLOW_LINES ||
	    !perf_enabled())
		return;
	native_slow_lines++;

	/* The draws' own costs. */
	texture = 0;
	copy = 0;
	program = 0;
	allocation = 0;
	for (draw = pass->first; draw != NULL; draw = draw->next) {
		texture += draw->texture_ticks;
		copy += draw->copy_ticks;
		program += draw->program_ticks;
		for (index = 0; index < draw->count; index++)
			allocation += draw->storage[index]->allocation_ticks;
	}
	for (index = 0; index < pass->count; index++)
		allocation += pass->storage[index]->allocation_ticks;

	/* One line for the pass. */
	perf_append(
		"v3d: slow pass op=%u draws=%u size=%ux%u us prep=%llu exec=%llu tex=%llu copy=%llu prog=%llu alloc=%llu tfu=%llu/%u bin=%llu render=%llu\n",
		opcode,
		pass->draws,
		pass->state.width,
		pass->state.height,
		(unsigned long long)average_us(prepare, 1),
		(unsigned long long)average_us(execute, 1),
		(unsigned long long)average_us(texture, 1),
		(unsigned long long)average_us(copy, 1),
		(unsigned long long)average_us(program, 1),
		(unsigned long long)average_us(allocation, 1),
		(unsigned long long)average_us(completion->tfu_ticks, 1),
		completion->tfu_jobs,
		(unsigned long long)average_us(completion->bin_ticks, 1),
		(unsigned long long)average_us(completion->render_ticks, 1));

	/* The pass's output and the textures the unit converted for it: their run, size, and the run's generation. */
	perf_append(
		"v3d: slow pass output=%llx",
		(unsigned long long)(uintptr_t)(pass->output != NULL ? pass->output->buffer : NULL) & 0xffffffULL);
	for (draw = pass->first; draw != NULL; draw = draw->next) {
		for (index = 0; index < draw->texture_count; index++) {
			if (!draw->textures[index].convert || draw->textures[index].snapshot == NULL)
				continue;
			perf_append(
				" tfu=%llx:%ux%u:g%llu",
				(unsigned long long)(uintptr_t)draw->textures[index].snapshot->buffer & 0xffffffULL,
				draw->textures[index].snapshot->width,
				draw->textures[index].snapshot->height,
				(unsigned long long)draw->textures[index].snapshot->generation);
		}
	}
	perf_append("\n");
}

/*
 * Formats one line of the cost report into the text the executor logs once
 * it has given up the controller mutex; nothing without v3d.perf=1, and a
 * line that does not fit is cut.
 */
static void
perf_append(
	const char *format,
	...)
{
	va_list args;
	size_t room;
	int rendered;

	/* Nothing to format without the report, or without room. */
	if (!perf_enabled())
		return;
	room = sizeof(native_perf_text) - native_perf_used;
	if (room <= 1U)
		return;

	/* The line after the ones before. */
	va_start(args, format);
	rendered = kern_vsnprintf(native_perf_text + native_perf_used, room, format, args);
	va_end(args);
	if (rendered <= 0)
		return;

	/* A cut line keeps what fitted. */
	if ((size_t)rendered >= room)
		rendered = (int)(room - 1U);
	native_perf_used += (size_t)rendered;
}

/*
 * Writes the cost report a step formatted to the log, after the executor
 * gave up the controller mutex (the console's output then holds up no
 * ioctl), and empties it.
 */
void
bcm2711_vulkan_native_perf_flush(
	void)
{
	struct bcm2711_board_state board;

	/* Nothing waits. */
	if (native_perf_used == 0U)
		return;

	/* The report as one record; a cut last line still ends its record. */
	kern_log_write(native_perf_text, native_perf_used);
	if (native_perf_text[native_perf_used - 1U] != '\n')
		kern_log_write("\n", 1U);
	native_perf_used = 0U;

	/*
	 * The board's clocks, temperature and throttling with each report, so
	 * that runs measured at different CPU speeds can be told apart.  Asked
	 * here, outside the controller's mutex, once a window (a slow-pass line
	 * alone does not ask: the firmware's answer is slow).
	 */
	if (!native_perf_window)
		return;
	native_perf_window = false;
	bcm2711_board_state_read(&board);
	kern_logf("v3d: board arm=%u/%u core=%u v3d=%u MHz temp=%u.%u throttled=0x%05x\n",
		  (unsigned)(board.arm_hz / 1000000U),
		  (unsigned)(board.arm_measured_hz / 1000000U),
		  (unsigned)(board.core_hz / 1000000U),
		  (unsigned)(board.v3d_hz / 1000000U),
		  (unsigned)(board.milli_celsius / 1000U),
		  (unsigned)(board.milli_celsius % 1000U / 100U),
		  (unsigned)board.throttled);
}

/* Converts a tick total into microseconds per item, rounding down. */
static uint64_t
average_us(
	uint64_t ticks,
	uint64_t count)
{
	uint64_t microseconds;

	/* An empty window reports zero rather than dividing by zero. */
	if (count == 0)
		return 0;

	/*
	 * Scheduler ticks are coarse, but a total of many independently
	 * phased intervals still estimates the mean without bias.
	 */
	microseconds = kern_ticks_to_rate(ticks, 1000000U);

	/* Reports the average microseconds of one item. */
	return microseconds / count;
}
