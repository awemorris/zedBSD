/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Logical callbacks, whole pending graph ownership and checked native DMA retirement are separate boundaries. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/vulkan-native-job.h"
#include "drivers/gpu/bcm2711/vulkan-barrier.h"

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
	if (command == NULL || command->session == NULL || command->kind != I915_VK_OBJ_COMMAND_BUFFER)
		return EINVAL;
	session = command->session->render;
	if (session == NULL || session->device == NULL || session->vulkan != command->session || command->session->closing)
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
 * Executes one pending primary through complete sequential native passes under the controller mutex.
 *
 * Each following pass stages its coherent source bytes only after the
 * previous pass's actual native completion/output visibility.  A failed
 * launched pass keeps all native inputs and the whole pending primary in
 * this root.  Later callback disposal transfers it to controller quarantine
 * rather than dropping a last CPU pointer to still referenced DMA storage.
 */
int
bcm2711_vulkan_native_job_execute(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	void *payload,
	bool *retired)
{
	struct bcm2711_vulkan_native_job *job;
	const struct bcm2711_vulkan_prepared_event *event;
	const struct bcm2711_vulkan_prepared_event *next;
	struct bcm2711_v3d_job_result completion;
	uint64_t remaining;
	uint64_t bytes;
	int error;

	/* Initial refusals launch no work and therefore retain no device borrow. */
	if (retired == NULL)
		return EINVAL;
	*retired = true;
	job = payload;

	/* The accepted payload must belong to the same exact renderer namespace and native controller. */
	if (controller == NULL || session == NULL || job == NULL || job->prepared == NULL || job->session != session || session->device != controller || controller->space.native == NULL)
		return EINVAL;

	/* A completed or uncertain payload never replays coherent source reads, clear or native work. */
	if (job->executed || job->quarantined) {
		*retired = job->retired;
		return EBUSY;
	}

	/* A stopped controller refuses both native launches and CPU layout publication before the primary becomes single-use work. */
	if (!controller->space.native->hardware.initialized ||
	    !controller->space.native->hardware.ready ||
	    !controller->space.native->power.ready)
		return EIO;

	/* One finite resident upload budget covers every pass and its complete independent draw input prefix. */
	job->executed = true;
	remaining = NATIVE_JOB_BYTES;
	event = job->prepared->first;
	while (event != NULL) {
		/* Every earlier native pass completed before this explicit dependency can publish FIFO-visible layout state. */
		if (event->opcode == GPU_OP_CMD_PIPELINE_BARRIER) {
			error = bcm2711_vulkan_barrier_run(event->record);
			if (error != 0)
				return error;
			event = event->next;
			continue;
		}

		/* Both graphics and full-image clear meta passes use the same independent native owner and checked retirement path. */
		if (event->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
			error = bcm2711_vulkan_native_pass_create(&controller->space, event, &remaining, &job->pass, &next);
		} else if (event->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
			error = bcm2711_vulkan_native_clear_create(&controller->space, event, &remaining, &job->pass);
			next = event->next;
		} else {
			return ENOTSUP;
		}

		/* A refused whole-pass preparation owns no launched DMA and leaves the primary pending until disposal. */
		if (error != 0)
			return error;

		/* Exact clear, supervised bin/render and GPU cache visibility execute while every independent owner is held. */
		error = bcm2711_vulkan_native_pass_run(job->pass, &completion);
		job->retired = completion.retired;
		*retired = completion.retired;
		if (error != 0)
			return error;

		/* Confirmed DMA retirement permits this resident pass budget and its mappings to be reused by the next FIFO pass. */
		bytes = job->pass->bytes;
		error = bcm2711_vulkan_native_pass_release(&job->pass, true);
		if (error != 0)
			return error;
		remaining += bytes;
		event = next;
	}

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
	if (controller == NULL || job == NULL || job->session == NULL || job->session->device != controller)
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
