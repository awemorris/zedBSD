/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exclusive direct displays with private front/back images and a guest clock.
 * This clock is virtual, it does not claim synchronization to host monitor vblank.
 */

#include "internal.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>

#include <uapi/errno.h>

#define VENUS_DISPLAY_REFRESH		50000U
#define VENUS_DISPLAY_MAX_REFRESH	(KERN_CLOCK_HZ * 1000U)
#define VENUS_EDID_BYTES		1024U
#define VENUS_PROTOCOL_SCANOUTS		16U
#define VENUS_DISPLAY_EXTENT		4096U
#define VENUS_CONSOLE_STOP_TICKS	(15U * KERN_CLOCK_HZ)

/*
 * One validated progressive EDID timing retained until the next topology query.
 * The controller mutex protects this dynamically sized per-output list.
 */
struct venus_display_timing {
	struct venus_display_timing *next;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
};

/*
 * One protocol scanout, whose private buffers never escape through GPU handles.
 * The owning open remains live through each serialized callback.
 */
struct venus_display_output {
	struct venus_session *owner;
	struct venus_resource *front;
	struct venus_resource *back;
	struct venus_share *shared_front;
	struct venus_display_timing *timings;
	uint32_t mode_count;
	uint32_t preferred_refresh;
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;
	uint64_t generation;
	uint64_t lease;
	uint64_t sequence;
	uint64_t present_tick;
	uint32_t identifier;
	uint32_t preferred_width;
	uint32_t preferred_height;
	uint32_t current_width;
	uint32_t current_height;
	unsigned connected;
};

/* One dynamically allocated output inventory owned by its controller. */
struct venus_display_engine {
	struct venus_display_output *outputs;
	uint32_t count;
	uint64_t next_lease;
	uint64_t next_fence;
	unsigned initialized;

	/*
	 * Console storage belongs to the controller's context-zero namespace, never to an open.
	 * The worker accesses these fields under the controller mutex except its atomic stop flag.
	 */
	struct venus_display_output console;
	struct venus_session console_session;
	struct venus_resource *console_source;
	struct thread *console_worker;
	struct spinlock console_lock;
	struct wait_queue console_waiters;
	struct kern_text_observer console_observer;
	uint32_t console_generation;
	unsigned console_active;
	unsigned console_observing;
	volatile unsigned console_stopping;
};

static int display_console_start(struct venus_controller *controller);
static int display_console_update(struct venus_controller *controller);
static void display_console_worker(void *argument);
static int display_device_query(void *device, void *private_session, struct gpu_device_info *request);
static int display_constraints(void *device, void *private_session, struct gpu_scanout_constraints *request);
static int display_query(void *device, void *session, struct gpu_display_info *request);
static int display_mode(void *device, void *session, struct gpu_display_mode *request);
static int display_claim(void *device, void *session, struct gpu_display_claim *request);
static int display_release(void *device, void *session, const struct gpu_display_release *request);
static int display_present(void *device, void *session, void *object, struct gpu_display_present *request);
static int display_wait(void *device, void *session, struct gpu_display_wait *request);
static int display_events(void *device, void *session, uint64_t *sequence);
static int display_refresh(struct venus_controller *controller);
static int display_find(struct venus_controller *controller, uint32_t identifier, uint64_t generation, struct venus_display_output **result);
static int display_find_lease(struct venus_controller *controller, struct venus_session *session, uint64_t lease, struct venus_display_output **result);
static int display_validate_mode(uint32_t width, uint32_t height, uint32_t refresh);
static int display_release_output(struct venus_controller *controller, struct venus_display_output *output);
static int display_prepare(struct venus_controller *controller, struct venus_display_output *output, const struct gpu_display_present *request);
static int display_frame(struct venus_controller *controller, struct venus_display_output *output, struct venus_resource *source, struct gpu_display_present *request);
static int display_blob_frame(struct venus_controller *controller, struct venus_display_output *output, struct venus_resource *source, struct gpu_display_present *request);
static int display_fenced(struct venus_controller *controller, uint8_t *command, uint32_t bytes);
static int display_next_refresh(struct venus_controller *controller, struct venus_display_output *output, uint32_t refresh);
static int display_timings_refresh(struct venus_controller *controller, struct venus_display_output *output);
static int display_edid_parse(struct venus_display_output *output, const uint8_t *edid, uint32_t bytes);
static int display_edid_timing(struct venus_display_output *output, const uint8_t *descriptor);
static void display_timings_free(struct venus_display_output *output);
static int display_status(struct venus_controller *controller, const void *command, uint32_t command_bytes, void *reply, uint32_t reply_bytes, uint32_t expected);

/* Hardware callbacks share the controller mutex; event snapshots use only the IRQ-safe queue lock. */
const struct drv_gpu_display_ops drv_venus_display_operations = {
	display_query, display_mode, display_claim,
	display_release, display_present, display_wait, display_events
};

/* Native sharing is supported only within this Venus controller's resource namespace. */
const struct drv_gpu_scanout_ops drv_venus_scanout_operations = {
	display_device_query, display_constraints, NULL
};

/*
 * Ends all leases belonging to an open before its context and wrapper retire.
 */
void
drv_venus_display_close_locked(
	struct venus_controller *controller,
	struct venus_session *session)
{
	struct venus_display_output *output;
	uint32_t index;
	int error;

	/* Opens which never touched display state have no display cleanup. */
	if (controller->display == NULL)
		return;

	/* Every output is independent, including its private backing ownership. */
	for (index = 0U; index < controller->display->count; index++) {
		output = &controller->display->outputs[index];
		if (output->owner != session)
			continue;

		/* Uncertain hardware references remain controller-owned until reset. */
		error = display_release_output(controller, output);
		if (error != 0) {
			drv_venus_transport_fail(&controller->transport, error);
			kern_logf("venus: display %u retained for reset: %d\n", output->identifier, error);

			/* Failed scanout storage remains on the controller list after its software hold retires. */
			if (output->shared_front != NULL) {
				drv_venus_share_put_locked(controller, output->shared_front);
				output->shared_front = NULL;
			}
		}

		/* A retired wrapper must never remain in lease arbitration. */
		output->owner = NULL;
		output->lease = 0U;

		/* Primary owner loss releases the sleeping console even after failed scanout retirement. */
		if (output->identifier == 1U)
			drv_venus_display_console_changed_locked(controller);
	}

	/* Succeeded: no display record refers to the closing session. */
	return;
}

/*
 * Drops a quarantined open's lease records without any host traffic.
 *
 * Its scanout hardware state remains whatever the isolated context left until
 * checked reset; only the arbitration pointers and the console gate change.
 */
void
drv_venus_display_forget_locked(
	struct venus_controller *controller,
	struct venus_session *session)
{
	struct venus_display_output *output;
	uint32_t index;

	/* Opens which never touched display state have no display cleanup. */
	if (controller->display == NULL)
		return;

	/* A quarantined worker may never answer, so no release command is sent for its outputs. */
	for (index = 0U; index < controller->display->count; index++) {
		output = &controller->display->outputs[index];
		if (output->owner != session)
			continue;

		/* The record leaves arbitration; its retained backing is retired by reset. */
		output->owner = NULL;
		output->lease = 0U;

		/* Primary owner loss releases the sleeping console gate. */
		if (output->identifier == 1U)
			drv_venus_display_console_changed_locked(controller);
	}

	/* Succeeded: no display record refers to the quarantined session. */
	return;
}

/*
 * Frees metadata after transport reset has already retired every resource.
 */
void
drv_venus_display_finish(
	struct venus_controller *controller)
{
	uint32_t index;

	/* Controller detach calls this only after checked device reset. */
	if (controller->display == NULL)
		return;

	/* Timing metadata owns no hardware reference and retires with its output inventory. */
	for (index = 0U; index < controller->display->count; index++)
		display_timings_free(&controller->display->outputs[index]);

	/* Resource pointers are intentionally not dereferenced after reset cleanup. */
	kern_free(controller->display->outputs);
	kern_free(controller->display);
	controller->display = NULL;

	/* Succeeded: no display metadata survives controller destruction. */
	return;
}

/*
 * Stops and reaps the console worker before any controller resource can be reset or freed.
 */
int
drv_venus_display_stop(
	struct venus_controller *controller)
{
	struct venus_display_engine *engine;
	struct thread *worker;
	unsigned long irq;
	uint64_t deadline;
	uint64_t now;
	int error;

	/* A controller which never claimed the primary display has no worker lifetime. */
	engine = controller->display;
	if (engine == NULL)
		return 0;

	/* Only the PCI withdrawal owner reaches this path after all GPU opens have retired. */
	worker = engine->console_worker;
	if (worker == NULL)
		return 0;

	/* The stop request prevents new work after any already-running finite transport call. */
	__atomic_store_n(&engine->console_stopping, 1U, __ATOMIC_RELEASE);

	/* Unlink first so text writers cannot access the queue after its controller retires. */
	kern_text_unobserve(&engine->console_observer);
	engine->console_observing = 0U;
	irq = spin_lock_irqsave(&engine->console_lock);

	/* The stop predicate and sequence handoff also cover a worker about to register its sleep. */
	waitq_wake_all(&engine->console_waiters);

	spin_unlock_irqrestore(&engine->console_lock, irq);

	/* Only the reap loop uses a deadline, the parked console itself has no periodic wake. */
	now = sched_ticks();
	if (now > UINT64_MAX - VENUS_CONSOLE_STOP_TICKS)
		return EOVERFLOW;

	/* The same absolute deadline bounds all reap retries without extending a stalled lifetime. */
	deadline = now + VENUS_CONSOLE_STOP_TICKS;

	/* A failed reap preserves both controller and thread ownership for a later detach retry. */
	while (1) {
		/* Reap consumes the thread only after its final controller access has ended. */
		error = thread_wait(worker, NULL);
		if (error == 0)
			break;

		/* Only a still-running worker is a retryable state. */
		if (error != EBUSY)
			return error;

		/* Do not free DMA while a stalled worker can still access its controller. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* A short ordinary sleep lets the requested worker leave its current wait and return. */
		sched_sleep(now + 1U);
	}

	/* A successful reap consumes the creator's only thread reference. */
	engine->console_worker = NULL;

	/* Succeeded: controller reset can no longer race console submission. */
	return 0;
}

/*
 * Keeps the older scanout-zero command from bypassing native lease ownership.
 */
int
drv_venus_display_legacy_available_locked(
	struct venus_controller *controller)
{
	/* An uninitialized direct-display engine cannot own a native plane. */
	if (controller->display == NULL)
		return 0;

	/* The legacy interface addresses only protocol scanout zero. */
	if (controller->display->count != 0U) {
		if (controller->display->outputs[0].owner != NULL)
			return EBUSY;
	}

	/* A subsequent legacy frame replaces the console even when no text has changed. */
	controller->display->console_active = 0U;

	/* Succeeded: legacy arbitration may now inspect its own session owner. */
	return 0;
}

/*
 * Reconfigures primary-console notifications after native or legacy ownership changes.
 * The caller holds the controller mutex, and never joins the worker from this path.
 */
void
drv_venus_display_console_changed_locked(
	struct venus_controller *controller)
{
	struct venus_display_engine *engine;
	unsigned long irq;
	unsigned stopping;
	int owned;
	int error;

	/* Controllers without a console worker own no subscription or initialized condition queue. */
	engine = controller->display;
	if (engine == NULL)
		return;

	/* Snapshot support and the first primary claim create this lifetime lazily. */
	if (engine->console_worker == NULL)
		return;

	/* Only scanout zero can hide this console, another output's owner is unrelated. */
	owned = 0;
	if (controller->display_owner != NULL)
		owned = 1;

	/* An empty inventory has no primary output available for retained text. */
	if (engine->count == 0U)
		owned = 1;
	else if (engine->outputs[0].owner != NULL)
		owned = 1;

	/* Teardown must not allow a racing ownership release to rearm text notifications. */
	stopping = __atomic_load_n(&engine->console_stopping, __ATOMIC_ACQUIRE);
	if (stopping != 0U)
		owned = 1;

	/* Unsubscribe outside the leaf condition lock to preserve registry-to-condition lock order. */
	if (owned != 0 && engine->console_observing != 0U) {
		kern_text_unobserve(&engine->console_observer);
		engine->console_observing = 0U;
	}

	/* Registration precedes the worker's generation check, covering text written during ownership. */
	if (owned == 0 && engine->console_observing == 0U) {
		error = kern_text_observe(
			&engine->console_observer,
			&engine->console_lock,
			&engine->console_waiters);
		if (error != 0) {
			kern_logf("venus: console notification unavailable: %d\n", error);
			return;
		}

		/* The controller mutex protects subscription ownership from another lease callback. */
		engine->console_observing = 1U;
	}

	/* Ownership changes use the same sequence as text changes and the detach stop request. */
	irq = spin_lock_irqsave(&engine->console_lock);

	waitq_wake_all(&engine->console_waiters);

	spin_unlock_irqrestore(&engine->console_lock, irq);

	/* Succeeded: the reusable worker can recheck the primary ownership predicate. */
	return;
}

/* Reports the controller's roles without claiming unrelated devices as companions. */
static int
display_device_query(
	void *device,
	void *private_session,
	struct gpu_device_info *request)
{
	/* Registration assigns identity; this backend combines rendering and native display. */
	(void)device;
	(void)private_session;
	request->roles = GPU_DEVICE_RENDER | GPU_DEVICE_DISPLAY;
	request->companion_id = 0U;

	/* Succeeded: there is no unverified cross-controller pairing promise. */
	return 0;
}

/* Publishes scanout requirements without treating host Vulkan memory as foreign DMA pages. */
static int
display_constraints(
	void *device,
	void *private_session,
	struct gpu_scanout_constraints *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	int error;

	/* The queried generation must still name this controller's connected output. */
	(void)private_session;
	controller = device;
	mutex_lock(&controller->mutex);

	/* Resolve the exact output generation before using its native ownership or constraints. */
	error = display_find(controller, request->display_id, request->generation, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Real blob import validates the source controller; ordinary copied storage also remains valid. */
	request->flags = GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->stride_alignment = 4U;
	request->offset_alignment = 4U;
	request->placement = 0U;
	request->max_dma_address = 0U;

	mutex_unlock(&controller->mutex);

	/* Succeeded: callers may trial same-controller sharing without CPU address assumptions. */
	return 0;
}

/* Creates one controller-owned console worker when the current text backend can snapshot cells. */
static int
display_console_start(
	struct venus_controller *controller)
{
	struct venus_display_engine *engine;
	struct kern_text_snapshot snapshot;
	struct thread *worker;
	int error;

	/* One worker serves the controller's primary output for the rest of its attached lifetime. */
	engine = controller->display;
	if (engine->console_worker != NULL)
		return 0;

	/* Optional text support cannot prevent graphics use on a platform without retained cells. */
	kern_memset(&snapshot, 0, sizeof(snapshot));
	error = kern_text_snapshot(&snapshot);
	if (error == ENOTSUP || error == ENODEV)
		return 0;

	/* A malformed or failed available snapshot is reported before admitting display ownership. */
	if (error != 0)
		return error;

	/* The private display and its source use context zero rather than a closing application's context. */
	engine->console_session.context = 0U;
	engine->console.identifier = 1U;
	engine->console.owner = &engine->console_session;
	engine->console_stopping = 0U;

	/* The leaf wake lock is shared only by text notification, ownership changes and worker sleep. */
	spin_init(&engine->console_lock, LOCK_RANK_POLL, "Venus console wake");
	waitq_init(&engine->console_waiters, "Venus console changes");

	/* No notification can reference this engine until the worker pointer is published. */
	error = kthread_create(
		display_console_worker,
		controller,
		SCHED_PRIORITY_DEFAULT,
		&worker);
	if (error != 0)
		return error;

	/* Publication precedes scheduling, and the current claim retains the controller mutex. */
	engine->console_worker = worker;
	thread_start(worker);

	/* Succeeded: future lease release can restore text without resetting unrelated Vulkan contexts. */
	return 0;
}

/* Paints a complete retained text image only when no application owns the primary scanout. */
static int
display_console_update(
	struct venus_controller *controller)
{
	unsigned failed;
	struct venus_display_engine *engine;
	struct kern_text_snapshot snapshot;
	struct gpu_display_present request;
	uint64_t bytes;
	uint32_t generation;
	int error;

	/* Transport failure leaves all display backing quarantined for the existing reset owner. */
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed != 0U)
		return ENODEV;

	/* A claimed native plane or legacy session has exclusive authority over its displayed image. */
	engine = controller->display;
	if (engine->count == 0U)
		return 0;

	/* No console transfer can overtake a claimed application's next present. */
	if (engine->outputs[0].owner != NULL || controller->display_owner != NULL)
		return 0;

	/* Equality skips an unchanged text image only while the console still owns scanout zero. */
	generation = kern_text_generation();
	if (engine->console_active != 0U && generation == engine->console_generation)
		return 0;

	/* A geometry query is based on retained cells, never a read of the abandoned firmware framebuffer. */
	kern_memset(&snapshot, 0, sizeof(snapshot));
	error = kern_text_snapshot(&snapshot);
	if (error != 0)
		return error;

	/* Console dimensions follow the existing grid and the ordinary bounded 2D presentation contract. */
	error = display_validate_mode(snapshot.width, snapshot.height, VENUS_DISPLAY_REFRESH);
	if (error != 0)
		return error;

	/* The source remains a tightly packed BGRA image owned by the controller. */
	if (snapshot.stride != snapshot.width * sizeof(uint32_t))
		return EIO;
	bytes = (uint64_t)snapshot.stride * snapshot.height;

	/* Retires only unused source memory when a replaced text backend changes the grid dimensions. */
	if (engine->console_source != NULL) {
		/* Equal-sized snapshots can keep the previous source without changing allocation ownership. */
		if (engine->console_source->bytes != bytes) {
			/* Only acknowledged retirement consumes the controller's previous source reference. */
			error = drv_venus_resource_release_locked(controller, engine->console_source);
			if (error != 0)
				return error;

			/* Null marks that the next snapshot must acquire its own complete backing extent. */
			engine->console_source = NULL;
		}
	}

	/* Partial allocation failure leaves any previously visible console front image intact. */
	if (engine->console_source == NULL) {
		error = drv_venus_storage_create_locked(
			controller,
			&engine->console_session,
			bytes,
			&engine->console_source);
		if (error != 0)
			return error;
	}

	/* The text lock is held only inside the snapshot call and never during a GPU command or wait. */
	snapshot.pixels = engine->console_source->backing.address;
	snapshot.bytes = (size_t)engine->console_source->bytes;
	error = kern_text_snapshot(&snapshot);
	if (error != 0)
		return error;

	/* The same private front/back transfer path preserves full-frame visibility and checked fences. */
	kern_memset(&request, 0, sizeof(request));
	request.width = snapshot.width;
	request.height = snapshot.height;
	request.stride = snapshot.stride;
	request.format = GPU_PIXEL_BGRA8888;
	request.refresh_millihz = VENUS_DISPLAY_REFRESH;
	request.flags = GPU_DISPLAY_PRESENT_FIFO;
	error = display_frame(controller, &engine->console, engine->console_source, &request);
	if (error != 0)
		return error;

	/* Text written after the sampled generation remains dirty for the next worker pass. */
	engine->console_generation = generation;
	engine->console_active = 1U;

	/* Succeeded: scanout zero again shows retained console cells and its current cursor. */
	return 0;
}

/* Services changed text outside console spinlocks and without interfering with active graphics owners. */
static void
display_console_worker(
	void *argument)
{
	struct venus_controller *controller;
	struct venus_display_engine *engine;
	unsigned long irq;
	uint64_t observed;
	unsigned stopping;
	int previous_error;
	int error;

	/* The PCI controller outlives this worker through its explicit stop-and-reap barrier. */
	controller = argument;
	engine = controller->display;
	previous_error = 0;

	/* Each pass consumes ownership or text changes and then parks without a timer. */
	while (1) {
		/* Observe before checking text or ownership so a change during rendering cannot be lost. */
		irq = spin_lock_irqsave(&engine->console_lock);

		observed = waitq_sequence(&engine->console_waiters);
		stopping = __atomic_load_n(&engine->console_stopping, __ATOMIC_ACQUIRE);

		spin_unlock_irqrestore(&engine->console_lock, irq);

		/* Stop is independent from the controller mutex and interrupts a parked worker. */
		if (stopping != 0U)
			break;

		/* Display ownership checks and complete GPU transactions share the ordinary backend mutex. */
		mutex_lock(&controller->mutex);

		error = display_console_update(controller);

		mutex_unlock(&controller->mutex);

		/* Reports a changed failure once; a transient allocation or absent backend can recover later. */
		if (error != 0 && error != previous_error)
			kern_logf("venus: console update deferred: %d\n", error);

		/* The next pass logs only a new failure, while success permits a later recurrence report. */
		previous_error = error;

		/* Wait registration shares the leaf lock with every possible notification source. */
		irq = spin_lock_irqsave(&engine->console_lock);

		/* A stop after the earlier snapshot must never become an uninterruptible idle sleep. */
		stopping = __atomic_load_n(&engine->console_stopping, __ATOMIC_ACQUIRE);
		if (stopping == 0U)
			(void)waitq_sleep(&engine->console_waiters, &engine->console_lock, observed, 0U, 0U);

		spin_unlock_irqrestore(&engine->console_lock, irq);
	}

	/* Succeeded: the kernel thread trampoline will publish exit for the detach owner to reap. */
	return;
}

/* Returns an inventory snapshot without reserving or changing any display. */
static int
display_query(
	void *device,
	void *private_session,
	struct gpu_display_info *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	int error;

	/* Discovery is read-only and belongs to the controller rather than a context. */
	(void)private_session;
	controller = device;
	mutex_lock(&controller->mutex);
	error = display_refresh(controller);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Count-only discovery needs no output index or persistent userspace object. */
	request->count = controller->display->count;
	if (request->index == GPU_DISPLAY_COUNT_ONLY) {
		mutex_unlock(&controller->mutex);
		return 0;
	}

	/* Ordinals enumerate hardware outputs; identities remain stable across modes. */
	if (request->index >= controller->display->count) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}
	output = &controller->display->outputs[request->index];
	request->display_id = output->identifier;
	request->generation = output->generation;
	request->flags = GPU_DISPLAY_VIRTUAL_CLOCK | GPU_DISPLAY_FIFO |
	    GPU_DISPLAY_ATOMIC_MODE_PRESENT | GPU_DISPLAY_BLOB;
	if (output->connected != 0U)
		request->flags |= GPU_DISPLAY_CONNECTED;
	if (output->front != NULL || output->shared_front != NULL)
		request->flags |= GPU_DISPLAY_ACTIVE;
	if (request->index == 0U && controller->primary_scanout != NULL)
		request->flags |= GPU_DISPLAY_ACTIVE;

	/* One opaque full-output plane accepts either defined packed pixel format. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = VENUS_MAX_RESOURCE_BYTES;
	request->current_width = output->current_width;
	request->current_height = output->current_height;

	/* Primary scanout geometry follows completed selection, including the unleased kernel console. */
	if (request->index == 0U) {
		request->current_width = controller->primary_width;
		request->current_height = controller->primary_height;
	}

	/* Preferred geometry remains independent from the current selected frame. */
	request->preferred_width = output->preferred_width;
	request->preferred_height = output->preferred_height;
	request->max_width = VENUS_DISPLAY_EXTENT;
	request->max_height = VENUS_DISPLAY_EXTENT;
	request->refresh_millihz = output->preferred_refresh;
	request->physical_width_mm = output->physical_width_mm;
	request->physical_height_mm = output->physical_height_mm;
	kern_snprintf(request->name, sizeof(request->name), "Venus virtual display %u", request->index);
	mutex_unlock(&controller->mutex);

	/* Succeeded: the caller has a bounded, generation-tagged display snapshot. */
	return 0;
}

/* Enumerates discovered timings or validates a supported virtual extent and clock. */
static int
display_mode(
	void *device,
	void *private_session,
	struct gpu_display_mode *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	struct venus_display_timing *timing;
	uint32_t index;
	int error;

	/* Mode discovery observes the same topology generation as display ownership. */
	(void)private_session;
	controller = device;
	mutex_lock(&controller->mutex);

	error = display_find(controller, request->display_id, request->generation, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Enumeration always includes the GET_DISPLAY_INFO preferred rectangle first. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = output->mode_count;
		if (request->index == GPU_DISPLAY_COUNT_ONLY) {
			mutex_unlock(&controller->mutex);
			return 0;
		}

		/* Out-of-range ordinals must not return uninitialized timing fields. */
		if (request->index >= output->mode_count) {
			mutex_unlock(&controller->mutex);
			return EINVAL;
		}

		/* An EDID timing matching the preferred rectangle supplies its actual nominal frequency. */
		request->width = output->preferred_width;
		request->height = output->preferred_height;
		request->refresh_millihz = output->preferred_refresh;
		index = 1U;
		timing = output->timings;

		/* Skip the already enumerated preferred timing and select one remaining actual DTD. */
		while (timing != NULL && request->index != 0U) {
			/* The preferred item is present exactly once in the public inventory. */
			if (timing->width != output->preferred_width ||
			    timing->height != output->preferred_height ||
			    timing->refresh != output->preferred_refresh) {
				/* Every additional ordinal refers to one complete validated EDID timing. */
				if (index == request->index) {
					request->width = timing->width;
					request->height = timing->height;
					request->refresh_millihz = timing->refresh;
					break;
				}

				/* Only distinct public modes consume an ordinal. */
				index++;
			}

			/* The list is retained under the controller mutex throughout this query. */
			timing = timing->next;
		}
	} else {
		/* Zero asks the driver to resolve a native frequency or its documented custom-mode fallback. */
		if (request->refresh_millihz == 0U) {
			request->refresh_millihz = VENUS_DISPLAY_REFRESH;
			timing = output->timings;

			/* The first exact geometry match retains the native EDID preference order. */
			while (timing != NULL) {
				/* Never borrow another mode's frequency for an unmatched custom extent. */
				if (timing->width == request->width && timing->height == request->height) {
					request->refresh_millihz = timing->refresh;
					break;
				}

				/* Absence of a matching native timing preserves custom virtual-mode compatibility. */
				timing = timing->next;
			}
		}

		/* Virtio selects a framebuffer rectangle; an explicit cadence need not describe a physical EDID mode. */
		error = display_validate_mode(request->width, request->height, request->refresh_millihz);
		if (error != 0) {
			mutex_unlock(&controller->mutex);
			return error;
		}
	}

	mutex_unlock(&controller->mutex);

	/* Succeeded: timing discovery and validation changed no selected scanout. */
	return 0;
}

/* Reserves a plane for this open without changing its current image or mode. */
static int
display_claim(
	void *device,
	void *private_session,
	struct gpu_display_claim *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	int error;

	/* All ownership decisions share one controller serialization point. */
	controller = device;
	mutex_lock(&controller->mutex);

	/* Resolve the exact output generation before using its native ownership or constraints. */
	error = display_find(controller, request->display_id, request->generation, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Each virtual output provides exactly one full-output plane. */
	if (request->plane_index != 0U) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}

	/* Neither repeated claims nor legacy ownership may alias an existing lease. */
	if (output->owner != NULL) {
		mutex_unlock(&controller->mutex);
		return EBUSY;
	}
	if (output->identifier == 1U && controller->display_owner != NULL) {
		mutex_unlock(&controller->mutex);
		return EBUSY;
	}

	/* Primary output restoration is controller-owned and prepared before any user lease exists. */
	if (output->identifier == 1U) {
		error = display_console_start(controller);
		if (error != 0) {
			mutex_unlock(&controller->mutex);
			return error;
		}
		controller->display->console_active = 0U;
	}

	/* Lease identities are never reused during the lifetime of this controller. */
	if (controller->display->next_lease == 0U) {
		mutex_unlock(&controller->mutex);
		return EOVERFLOW;
	}
	output->owner = private_session;
	output->lease = controller->display->next_lease++;
	output->sequence = 0U;
	output->present_tick = 0U;
	request->lease = output->lease;

	/* Text changes while the primary is claimed must not wake a worker to contend for this mutex. */
	if (output->identifier == 1U)
		drv_venus_display_console_changed_locked(controller);

	mutex_unlock(&controller->mutex);

	/* Succeeded: later mode changes and presentations require this lease. */
	return 0;
}

/* Releases one explicit lease independently of unrelated Vulkan resources. */
static int
display_release(
	void *device,
	void *private_session,
	const struct gpu_display_release *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	int error;

	/* Resolves the lease only within the submitting open's ownership. */
	controller = device;
	mutex_lock(&controller->mutex);
	error = display_find_lease(controller, private_session, request->lease, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* The lease is consumed only after scanout and both private images retire. */
	error = display_release_output(controller, output);
	mutex_unlock(&controller->mutex);
	if (error != 0)
		return error;

	/* Succeeded: another open may now reserve this output. */
	return 0;
}

/* Presents a whole copied frame after preparation and virtual refresh completion. */
static int
display_present(
	void *device,
	void *private_session,
	void *object,
	struct gpu_display_present *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	struct venus_resource *source;
	int error;

	/* GPU core resolves and pins the source resource before entering the backend. */
	controller = device;
	source = object;
	mutex_lock(&controller->mutex);
	error = display_find_lease(controller, private_session, request->lease, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* A topology transition cannot silently present through an obsolete surface. */
	error = display_refresh(controller);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}
	if (request->generation != output->generation) {
		mutex_unlock(&controller->mutex);
		return ESTALE;
	}
	if (output->connected == 0U) {
		mutex_unlock(&controller->mutex);
		return ENXIO;
	}

	/* Private storage and session ownership are independently checked in K. */
	if (source->controller != controller ||
	    source->context != output->owner->context) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}

	/* GPU-only presentation retains shared storage until the next selected frame or release. */
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U) {
		error = display_blob_frame(controller, output, source, request);
	} else {
		/* The existing copied route still consumes only ordinary guest storage. */
		if (source->kind != VENUS_RESOURCE_STORAGE) {
			mutex_unlock(&controller->mutex);
			return EINVAL;
		}

		error = display_frame(controller, output, source, request);
	}

	mutex_unlock(&controller->mutex);
	if (error != 0)
		return error;

	/* Succeeded: a complete image became current at the virtual display boundary. */
	return 0;
}

/* Observes an already submitted sequence without inventing future work. */
static int
display_wait(
	void *device,
	void *private_session,
	struct gpu_display_wait *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	int error;

	/* Present is synchronous, so its sequence is complete before it is returned. */
	controller = device;
	mutex_lock(&controller->mutex);
	error = display_find_lease(controller, private_session, request->lease, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* A caller cannot wait for a sequence which was never successfully submitted. */
	if (request->sequence > output->sequence) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}
	if (output->sequence == 0U) {
		mutex_unlock(&controller->mutex);
		return EAGAIN;
	}

	/* The timestamp describes the guest virtual clock, not host monitor vblank. */
	request->completed_sequence = output->sequence;
	request->present_time_ns = output->present_tick * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	request->generation = output->generation;
	mutex_unlock(&controller->mutex);

	/* Succeeded: selection is complete; a current shared blob remains retained for scanout. */
	return 0;
}

/* Samples driver-owned event state without waiting behind rendering or display commands. */
static int
display_events(
	void *device,
	void *session,
	uint64_t *sequence)
{
	struct venus_controller *controller;
	unsigned long irq;
	unsigned overflow;

	(void)session;

	/* The retained open keeps this controller and short event lock alive during poll. */
	controller = device;
	irq = spin_lock_irqsave(&controller->transport.queue_lock);

	*sequence = controller->transport.topology_sequence;
	overflow = controller->transport.topology_overflow;

	spin_unlock_irqrestore(&controller->transport.queue_lock, irq);

	/* A saturated notification stream must never silently acknowledge a later event. */
	if (overflow != 0U)
		return EOVERFLOW;

	/* Succeeded: no hardware command or event acknowledgement occurred in this snapshot. */
	return 0;
}

/* Refreshes native topology only when first queried or explicitly invalidated. */
static int
display_refresh(
	struct venus_controller *controller)
{
	unsigned failed;
	struct venus_display_engine *engine;
	struct venus_display_output *output;
	uint8_t command[24];
	uint8_t response[408];
	uint8_t *configuration;
	uint32_t count;
	uint32_t events;
	uint32_t index;
	uint32_t width;
	uint32_t height;
	unsigned connected;
	int error;

	/* No cached information can make a failed transport usable again. */
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed != 0U)
		return ENODEV;

	/* Scanout count is a protocol hardware bound, not a framework allocation limit. */
	configuration = controller->transport.configuration.mapping.address;
	count = kern_mmio_read32(configuration + 8U);
	if (count > VENUS_PROTOCOL_SCANOUTS)
		return EIO;

	/* Lazily allocates only the actual output inventory. */
	if (controller->display == NULL) {
		engine = kern_calloc(1U, sizeof(*engine));
		if (engine == NULL)
			return ENOMEM;
		if (count != 0U) {
			engine->outputs = kern_calloc(count, sizeof(*engine->outputs));
			if (engine->outputs == NULL) {
				kern_free(engine);
				return ENOMEM;
			}
		}
		engine->count = count;
		engine->next_lease = 1U;
		engine->next_fence = 1U;
		controller->display = engine;
	}

	/* Virtio fixes num_scanouts for the device lifetime; only topology may change. */
	engine = controller->display;
	if (count != engine->count)
		return EIO;
	events = kern_mmio_read32(configuration);
	if (engine->initialized != 0U && (events & 1U) == 0U)
		return 0;

	/* Publishes before clearing, including when a query outruns delayed MSI-X delivery. */
	if ((events & 1U) != 0U) {
		drv_venus_transport_display_changed(&controller->transport);
		kern_mmio_write32(configuration + 4U, 1U);
	}

	/* Queries after acknowledgement so later hardware changes remain latched independently. */
	drv_venus_header(command, 0x0100U, 0U);
	error = display_status(controller, command, sizeof(command), response, sizeof(response), 0x1101U);
	if (error != 0)
		return error;

	/* Every protocol entry contains rect x/y/width/height, enabled and flags. */
	for (index = 0U; index < count; index++) {
		output = &engine->outputs[index];
		width = drv_venus_load32(response + 32U + index * 24U);
		height = drv_venus_load32(response + 36U + index * 24U);
		connected = drv_venus_load32(response + 40U + index * 24U);
		if (connected != 0U)
			connected = 1U;

		/* Rejects host geometry which the documented backing extent cannot carry. */
		error = display_validate_mode(width, height, VENUS_DISPLAY_REFRESH);
		if (error != 0) {
			width = 320U;
			height = 240U;
			connected = 0U;
		}

		/* A topology event invalidates validated modes without replacing handles. */
		if (output->generation == UINT64_MAX)
			return EOVERFLOW;
		output->generation++;
		output->identifier = index + 1U;
		output->preferred_width = width;
		output->preferred_height = height;
		output->connected = connected;

		/* Optional EDID refines this generation without substituting for actual connection state. */
		error = display_timings_refresh(controller, output);
		if (error != 0)
			return error;
	}

	engine->initialized = 1U;

	/* Succeeded: discovery now reflects one completed native topology query. */
	return 0;
}

/* Resolves a connected display against the exact capabilities used by its caller. */
static int
display_find(
	struct venus_controller *controller,
	uint32_t identifier,
	uint64_t generation,
	struct venus_display_output **result)
{
	struct venus_display_output *output;
	int error;

	/* Topology observation occurs before generation and connection checks. */
	error = display_refresh(controller);
	if (error != 0)
		return error;
	if (identifier == 0U || identifier > controller->display->count)
		return EINVAL;
	output = &controller->display->outputs[identifier - 1U];
	if (generation != output->generation)
		return ESTALE;
	if (output->connected == 0U)
		return ENXIO;

	/* A checked output remains stable while the controller mutex is held. */
	*result = output;

	/* Succeeded: the caller may use this generation's capabilities. */
	return 0;
}

/* Resolves an unforgeable open-scoped lease even after a topology transition. */
static int
display_find_lease(
	struct venus_controller *controller,
	struct venus_session *session,
	uint64_t lease,
	struct venus_display_output **result)
{
	unsigned failed;
	struct venus_display_output *output;
	uint32_t index;

	/* A missing engine or reserved lease zero cannot describe ownership. */
	if (controller->display == NULL || lease == 0U)
		return EINVAL;
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed != 0U)
		return ENODEV;

	/* A foreign open cannot release, observe or replace another owner's frame. */
	for (index = 0U; index < controller->display->count; index++) {
		output = &controller->display->outputs[index];
		if (output->lease != lease)
			continue;
		if (output->owner != session)
			return EPERM;
		*result = output;
		return 0;
	}

	/* The identity was never claimed here or has already been consumed. */
	return EINVAL;
}

/* Validates independent framebuffer geometry and guest cadence without programming physical timings. */
static int
display_validate_mode(
	uint32_t width,
	uint32_t height,
	uint32_t refresh)
{
	uint64_t bytes;

	/* Nominal refresh above the guest tick rate cannot receive distinct FIFO boundaries. */
	if (refresh < 1000U || refresh > VENUS_DISPLAY_MAX_REFRESH)
		return EINVAL;

	/* Scanout selects a bounded resource rectangle independently of the optional EDID inventory. */
	if (width == 0U ||
	    height == 0U ||
	    width > VENUS_DISPLAY_EXTENT ||
	    height > VENUS_DISPLAY_EXTENT) {
		return EINVAL;
	}

	/* A maximum dimension alone cannot authorize an oversized full-frame backing. */
	bytes = (uint64_t)width * height * 4U;
	if (bytes > VENUS_MAX_RESOURCE_BYTES)
		return EINVAL;

	/* Succeeded: this whole-frame mode fits the native presentation contract. */
	return 0;
}

/* Withdraws scanout before returning any private backing to the allocator. */
static int
display_release_output(
	struct venus_controller *controller,
	struct venus_display_output *output)
{
	uint8_t command[48];
	int error;

	/* A completed disable ends the display's reference to its front image. */
	if (output->front != NULL || output->shared_front != NULL) {
		kern_memset(command, 0, sizeof(command));
		drv_venus_header(command, 0x0103U, 0U);
		drv_venus_store32(command + 40U, output->identifier - 1U);
		error = display_fenced(controller, command, sizeof(command));
		if (error != 0)
			return error;

		/* A completed disable ends the actual primary scanout independently from pending console redraw. */
		if (output->identifier == 1U) {
			controller->primary_scanout = NULL;
			controller->primary_width = 0U;
			controller->primary_height = 0U;
		}
	}

	/* Acknowledged scanout disable makes the shared source reusable by its producer. */
	if (output->shared_front != NULL) {
		drv_venus_share_put_locked(controller, output->shared_front);
		output->shared_front = NULL;
	}

	/* Each successful release consumes exactly one private resource pointer. */
	if (output->front != NULL) {
		error = drv_venus_resource_release_locked(controller, output->front);
		if (error != 0)
			return error;
		output->front = NULL;
	}
	if (output->back != NULL) {
		error = drv_venus_resource_release_locked(controller, output->back);
		if (error != 0)
			return error;
		output->back = NULL;
	}

	/* Only fully retired ownership becomes available to another open. */
	output->owner = NULL;
	output->lease = 0U;
	output->sequence = 0U;
	output->current_width = 0U;
	output->current_height = 0U;

	/* The primary scanout needs its retained console even when no new text was written. */
	if (output->identifier == 1U) {
		controller->display->console_active = 0U;

		/* Release wakes the parked console even when retained text has not changed. */
		drv_venus_display_console_changed_locked(controller);
	}

	/* Succeeded: no scanout or private image retains this lease. */
	return 0;
}

/* Prepares only the non-current image, preserving a complete visible front. */
static int
display_prepare(
	struct venus_controller *controller,
	struct venus_display_output *output,
	const struct gpu_display_present *request)
{
	struct gpu_present geometry;
	uint64_t bytes;
	int error;

	/* The private image uses tightly packed rows independent of the user stride. */
	bytes = (uint64_t)request->width * request->height * 4U;
	if (output->back != NULL) {
		if (output->back->bytes != bytes) {
			error = drv_venus_resource_release_locked(controller, output->back);
			if (error != 0)
				return error;
			output->back = NULL;
		}
	}

	/* No allocator failure can disturb the currently scanned front image. */
	if (output->back == NULL) {
		error = drv_venus_storage_create_locked(controller, output->owner, bytes, &output->back);
		if (error != 0)
			return error;
	}

	/* Initializes host geometry before any transfer or virtual display update. */
	kern_memset(&geometry, 0, sizeof(geometry));
	geometry.width = request->width;
	geometry.height = request->height;
	geometry.stride = request->width * 4U;
	geometry.format = request->format;
	error = drv_venus_storage_prepare_locked(controller, output->back, &geometry);
	if (error != 0)
		return error;

	/* Succeeded: the non-current image is ready for a complete pixel transfer. */
	return 0;
}

/* Copies, transfers and presents one whole frame as one serialized FIFO entry. */
static int
display_frame(
	struct venus_controller *controller,
	struct venus_display_output *output,
	struct venus_resource *source,
	struct gpu_display_present *request)
{
	struct venus_resource *previous;
	uint8_t command[56];
	uint8_t *destination;
	const uint8_t *pixels;
	uint64_t bytes;
	uint32_t row;
	int error;

	/* Every frame independently validates mode, packing and source bounds. */
	error = display_validate_mode(request->width, request->height, request->refresh_millihz);
	if (error != 0)
		return error;
	if (request->flags != GPU_DISPLAY_PRESENT_FIFO)
		return EINVAL;
	if (request->format != GPU_PIXEL_RGBA8888 && request->format != GPU_PIXEL_BGRA8888)
		return EINVAL;
	if (request->stride < request->width * 4U || (request->stride & 3U) != 0U)
		return EINVAL;
	bytes = (uint64_t)request->stride * request->height;
	if (request->offset > source->bytes)
		return EINVAL;
	if (bytes > source->bytes - request->offset)
		return EINVAL;
	if (output->sequence == UINT64_MAX)
		return EOVERFLOW;

	/* Host transfer targets a private image which is never currently displayed. */
	error = display_prepare(controller, output, request);
	if (error != 0)
		return error;
	destination = output->back->backing.address;
	pixels = (const uint8_t *)source->backing.address + (size_t)request->offset;
	for (row = 0U; row < request->height; row++) {
		kern_memcpy(
			destination + (size_t)row * request->width * 4U,
			pixels + (size_t)row * request->stride,
			(size_t)request->width * 4U);
	}

	/* A transport receipt alone cannot prove the GL transfer has completed. */
	kern_memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0105U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store32(command + 48U, output->back->identifier);
	kern_io_write_barrier();
	error = display_fenced(controller, command, sizeof(command));
	if (error != 0)
		return error;

	/* The queue consumes at most one completed image per virtual refresh tick. */
	error = display_next_refresh(controller, output, request->refresh_millihz);
	if (error != 0)
		return error;

	/* Geometry and the fully transferred image change in the same scanout command. */
	kern_memset(command, 0, 48U);
	drv_venus_header(command, 0x0103U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store32(command + 40U, output->identifier - 1U);
	drv_venus_store32(command + 44U, output->back->identifier);
	error = display_fenced(controller, command, 48U);
	if (error != 0)
		return error;

	/* Successful selection transfers the native front reference before any flush. */
	previous = output->front;
	output->front = output->back;
	output->back = previous;
	output->current_width = request->width;
	output->current_height = request->height;

	/* Query state follows the selected native or console resource, never a redraw hint. */
	if (output->identifier == 1U) {
		controller->primary_scanout = output->front;
		controller->primary_width = request->width;
		controller->primary_height = request->height;
	}

	/* Replacing a blob scanout ends its retained source lifetime after the host acknowledges selection. */
	if (output->shared_front != NULL) {
		drv_venus_share_put_locked(controller, output->shared_front);
		output->shared_front = NULL;
	}

	/* Completes the whole visible update before reporting a reusable source image. */
	kern_memset(command, 0, 48U);
	drv_venus_header(command, 0x0104U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store32(command + 40U, output->front->identifier);
	error = display_fenced(controller, command, 48U);
	if (error != 0)
		return error;

	/* Sequence publication occurs only after the complete virtual frame finishes. */
	output->sequence++;
	output->present_tick = sched_ticks();
	request->sequence = output->sequence;

	/* Succeeded: the native display owns its private frame, not caller storage. */
	return 0;
}

/* Selects a completed shared GPU image without any CPU pixel copy or upload. */
static int
display_blob_frame(
	struct venus_controller *controller,
	struct venus_display_output *output,
	struct venus_resource *source,
	struct gpu_display_present *request)
{
	struct venus_share *share;
	struct venus_share *previous;
	const struct gpu_image_descriptor *image;
	uint8_t command[96];
	uint32_t format;
	int error;

	/* Only explicitly exported image allocations possess immutable scanout metadata. */
	if (source->kind != VENUS_RESOURCE_BLOB || source->share == NULL)
		return EINVAL;

	/* Native mode validation remains identical to the existing FIFO display contract. */
	error = display_validate_mode(request->width, request->height, request->refresh_millihz);
	if (error != 0)
		return error;

	/* The blob bit selects the native image route without enabling unknown presentation modes. */
	if (request->flags != (GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB))
		return EINVAL;

	/* A caller cannot reinterpret an imported allocation beyond its exported image contract. */
	share = source->share;
	image = &share->image;
	if (request->width != image->width ||
	    request->height != image->height ||
	    request->stride != image->stride ||
	    request->offset != image->offset ||
	    request->format != image->format)
		return EINVAL;

	/* Sequence exhaustion must leave the previously selected complete image unchanged. */
	if (output->sequence == UINT64_MAX)
		return EOVERFLOW;

	/* Scanout keeps an allocation hold independently from all client and compositor aliases. */
	error = drv_venus_share_hold_locked(share);
	if (error != 0)
		return error;

	/* Preserve the same finite guest refresh pacing as ordinary native presentation. */
	error = display_next_refresh(controller, output, request->refresh_millihz);
	if (error != 0) {
		drv_venus_share_put_locked(controller, share);
		return error;
	}

	/* Translate the published channel order to virtio's packed scanout image format. */
	format = 67U;
	if (image->format == GPU_PIXEL_BGRA8888)
		format = 1U;

	/* The host imports the retained allocation directly into its GL display path. */
	kern_memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x010dU, 0U);
	drv_venus_store32(command + 32U, image->width);
	drv_venus_store32(command + 36U, image->height);
	drv_venus_store32(command + 40U, output->identifier - 1U);
	drv_venus_store32(command + 44U, share->storage->identifier);
	drv_venus_store32(command + 48U, image->width);
	drv_venus_store32(command + 52U, image->height);
	drv_venus_store32(command + 56U, format);
	drv_venus_store32(command + 64U, image->stride);
	drv_venus_store32(command + 80U, (uint32_t)image->offset);
	error = display_fenced(controller, command, sizeof(command));
	if (error != 0) {
		drv_venus_transport_fail(&controller->transport, error);
		drv_venus_share_put_locked(controller, share);
		return error;
	}

	/* Acknowledged selection transfers the current scanout hold before the old one retires. */
	previous = output->shared_front;
	output->shared_front = share;
	output->current_width = image->width;
	output->current_height = image->height;

	/* Console/query state follows the real selected blob rather than cached copied buffers. */
	if (output->identifier == 1U) {
		controller->primary_scanout = share->storage;
		controller->primary_width = image->width;
		controller->primary_height = image->height;
	}

	/* Previous producers may reuse their allocation only after it leaves the selected scanout. */
	if (previous != NULL)
		drv_venus_share_put_locked(controller, previous);

	/* Publish damage for the GPU-resident image without a transfer-to-host command. */
	kern_memset(command, 0, 48U);
	drv_venus_header(command, 0x0104U, 0U);
	drv_venus_store32(command + 32U, image->width);
	drv_venus_store32(command + 36U, image->height);
	drv_venus_store32(command + 40U, share->storage->identifier);
	error = display_fenced(controller, command, 48U);
	if (error != 0)
		return error;

	/* Completed presentation advances only after the host consumed the complete visible update. */
	output->sequence++;
	output->present_tick = sched_ticks();
	request->sequence = output->sequence;

	/* Succeeded: scanout retains this GPU allocation until replacement or explicit release. */
	return 0;
}

/* Requires an acknowledged fence for completed GL-context-zero display work. */
static int
display_fenced(
	struct venus_controller *controller,
	uint8_t *command,
	uint32_t bytes)
{
	uint8_t response[24];
	uint64_t fence;
	uint64_t returned_fence;
	uint32_t flags;
	int error;

	/* Monotonic fence identities cannot alias an earlier unfinished command. */
	if (controller->display->next_fence == 0U)
		return EOVERFLOW;
	fence = controller->display->next_fence++;
	drv_venus_store32(command + 4U, 1U);
	drv_venus_store64(command + 8U, fence);
	error = display_status(controller, command, bytes, response, sizeof(response), 0x1100U);
	if (error != 0)
		return error;

	/* Both the fence flag and its exact identity must survive the device reply. */
	flags = drv_venus_load32(response + 4U);
	if ((flags & 1U) == 0U) {
		drv_venus_transport_fail(&controller->transport, EIO);
		return EIO;
	}

	/* A completed fence must name this exact command rather than an earlier display update. */
	returned_fence = drv_venus_load64(response + 8U);
	if (returned_fence != fence) {
		drv_venus_transport_fail(&controller->transport, EIO);
		return EIO;
	}

	/* Succeeded: the host finished this display command before its reply. */
	return 0;
}

/* Reaches the nominal refresh boundary without stopping unrelated renderer sessions. */
static int
display_next_refresh(
	struct venus_controller *controller,
	struct venus_display_output *output,
	uint32_t refresh)
{
	unsigned failed;
	struct venus_session *owner;
	uint64_t lease;
	uint64_t generation;
	uint64_t now;
	uint64_t frame;
	uint64_t target;
	uint64_t ticks;

	/* Mode validation also bounds arithmetic and prevents multiple frames in one guest tick. */
	if (refresh < 1000U || refresh > VENUS_DISPLAY_MAX_REFRESH)
		return EINVAL;

	/* Nominal frame phase carries the fractional period instead of rounding every period upward. */
	now = sched_ticks();
	ticks = (uint64_t)KERN_CLOCK_HZ * 1000U;
	if (now > UINT64_MAX / refresh)
		return EOVERFLOW;
	frame = now * refresh / ticks + 1U;
	if (frame > (UINT64_MAX - (refresh - 1U)) / ticks)
		return EOVERFLOW;
	target = (frame * ticks + refresh - 1U) / refresh;

	/* Completion cannot move backward even when a caller changes its nominal mode. */
	if (now < output->present_tick)
		return EIO;

	/* Active callbacks or the joined console worker keep output storage alive across this wait. */
	owner = output->owner;
	lease = output->lease;
	generation = output->generation;

	/* Other sessions may submit rendering while this output awaits its guest refresh boundary. */
	mutex_unlock(&controller->mutex);

	/* Tick quantization preserves average cadence without claiming physical host vblank. */
	while (now < target) {
		sched_sleep(target);
		now = sched_ticks();
	}

	/* Revalidate exclusive output authority before allowing the caller to select any image. */
	mutex_lock(&controller->mutex);

	/* Failed transport cannot authorize scanout after a wait which admitted other sessions. */
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed != 0U)
		return ENODEV;

	/* A withdrawn or replaced lease cannot inherit this earlier reservation's pacing slot. */
	if (output->owner != owner ||
	    output->lease != lease ||
	    output->generation != generation)
		return ESTALE;

	/* Console rendering must yield if an application claimed either primary display route. */
	if (output == &controller->display->console) {
		/* This worker's private image must never overtake a new application-owned frame. */
		if (controller->display->outputs[0].owner != NULL || controller->display_owner != NULL)
			return EBUSY;
	}

	/* Succeeded: the unchanged owner reached its next distinct guest FIFO boundary. */
	return 0;
}

/* Checks one exact protocol response without accepting truncated success data. */
static int
display_status(
	struct venus_controller *controller,
	const void *command,
	uint32_t command_bytes,
	void *reply,
	uint32_t reply_bytes,
	uint32_t expected)
{
	uint32_t received;
	uint32_t response_type;
	int error;

	/* Transport owns persistent DMA storage across success and uncertain failure. */
	error = drv_venus_transport_command(
		&controller->transport,
		command,
		command_bytes,
		reply,
		reply_bytes,
		&received);
	if (error != 0)
		return error;

	/* A malformed success response cannot authorize a local ownership transition. */
	if (received != reply_bytes) {
		drv_venus_transport_fail(&controller->transport, EIO);
		return EIO;
	}

	/* Interprets the response tag only after its complete required interval was received. */
	response_type = drv_venus_load32(reply);
	if (response_type != expected) {
		drv_venus_transport_fail(&controller->transport, EIO);
		return EIO;
	}

	/* Succeeded: the caller may inspect the complete expected response. */
	return 0;
}

/* Refreshes one output's optional native EDID without making malformed metadata authoritative. */
static int
display_timings_refresh(
	struct venus_controller *controller,
	struct venus_display_output *output)
{
	unsigned failed;
	struct venus_display_timing *timing;
	uint8_t command[32];
	uint8_t response[32U + VENUS_EDID_BYTES];
	uint32_t bytes;
	int error;

	/* A topology generation must never inherit stale native timings from a previous monitor. */
	display_timings_free(output);
	output->mode_count = 1U;
	output->preferred_refresh = VENUS_DISPLAY_REFRESH;
	output->physical_width_mm = 0U;
	output->physical_height_mm = 0U;

	/* Connection comes from GET_DISPLAY_INFO even when the host retains an old EDID blob. */
	if (output->connected == 0U)
		return 0;

	/* Hosts without optional EDID retain the ordinary discovered-rectangle fallback. */
	if ((controller->transport.features & VENUS_FEATURE_EDID) == 0U)
		return 0;

	/* Virtio addresses EDID by its zero-based native scanout identifier. */
	kern_memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x010aU, 0U);
	drv_venus_store32(command + 24U, output->identifier - 1U);
	error = display_status(controller, command, sizeof(command), response, sizeof(response), 0x1104U);
	if (error != 0) {
		/* An uncertain transport is a device failure, not an optional metadata fallback. */
		failed = atomic_raw_load_acquire(&controller->transport.failed);
		if (failed != 0U)
			return error;

		/* A definite unsupported EDID response leaves the discovered virtual rectangle usable. */
		kern_logf("venus: display %u EDID unavailable: %d; using virtual timing\n", output->identifier, error);
		return 0;
	}

	/* The returned size bounds every EDID byte before header, extension or checksum access. */
	bytes = drv_venus_load32(response + 24U);
	error = display_edid_parse(output, response + 32U, bytes);
	if (error != 0) {
		/* Partial allocation never publishes a subset of a failed timing inventory. */
		display_timings_free(output);
		output->physical_width_mm = 0U;
		output->physical_height_mm = 0U;
		if (error == ENOMEM)
			return error;

		/* Malformed optional metadata cannot replace the already verified native rectangle. */
		kern_logf("venus: display %u EDID rejected: %d; using virtual timing\n", output->identifier, error);
		return 0;
	}

	/* The first real timing for the host-preferred rectangle supplies its nominal refresh. */
	timing = output->timings;
	while (timing != NULL) {
		/* GET_DISPLAY_INFO retains geometry authority; EDID refines its timing when available. */
		if (timing->width == output->preferred_width && timing->height == output->preferred_height) {
			output->preferred_refresh = timing->refresh;
			break;
		}

		/* A preferred rectangle without a matching DTD keeps the documented fallback frequency. */
		timing = timing->next;
	}

	/* Count each remaining actual timing once after removing the preferred-mode duplicate. */
	timing = output->timings;
	while (timing != NULL) {
		/* Separate frequencies for the same dimensions remain distinct public modes. */
		if (timing->width != output->preferred_width ||
		    timing->height != output->preferred_height ||
		    timing->refresh != output->preferred_refresh)
			output->mode_count++;

		/* Only controller-owned timing records contribute to enumeration. */
		timing = timing->next;
	}

	/* Succeeded: this generation exposes real supported nominal timings and physical dimensions. */
	return 0;
}

/* Parses complete checksummed EDID base and CTA detailed timing descriptors. */
static int
display_edid_parse(
	struct venus_display_output *output,
	const uint8_t *edid,
	uint32_t bytes)
{
	static const uint8_t header[8] = { 0U, 255U, 255U, 255U, 255U, 255U, 255U, 0U };
	uint32_t blocks;
	uint32_t block;
	uint32_t index;
	uint32_t offset;
	uint32_t checksum;
	const uint8_t *extension;
	int different;
	int error;

	/* The virtio reply carries at most eight complete 128-byte EDID blocks. */
	if (bytes < 128U ||
	    bytes > VENUS_EDID_BYTES ||
	    (bytes & 127U) != 0U)
		return EINVAL;

	/* The EDID signature and version distinguish timings from unrelated device data. */
	different = kern_memcmp(edid, header, sizeof(header));
	if (different != 0 ||
	    edid[18] != 1U ||
	    edid[19] > 4U)
		return EINVAL;

	/* The extension count may not make a parser walk beyond the host-reported extent. */
	blocks = (uint32_t)edid[126] + 1U;
	if (blocks > bytes / 128U)
		return EINVAL;

	/* Every declared block is authenticated by its EDID checksum before any timing is retained. */
	for (block = 0U; block < blocks; block++) {
		checksum = 0U;

		/* The sum includes the checksum byte and wraps in the defined eight-bit domain. */
		for (index = 0U; index < 128U; index++)
			checksum += edid[block * 128U + index];

		/* One malformed extension invalidates the entire optional timing inventory. */
		if ((checksum & 255U) != 0U)
			return EINVAL;
	}

	/* Only two nonzero EDID size fields describe physical centimeters rather than an aspect ratio. */
	if (edid[21] != 0U && edid[22] != 0U) {
		output->physical_width_mm = (uint32_t)edid[21] * 10U;
		output->physical_height_mm = (uint32_t)edid[22] * 10U;
	}

	/* The base block has four fixed descriptor slots, including possible non-timing records. */
	for (index = 0U; index < 4U; index++) {
		error = display_edid_timing(output, edid + 54U + index * 18U);
		if (error != 0)
			return error;
	}

	/* CTA extensions carry additional detailed timings after their variable data-block region. */
	for (block = 1U; block < blocks; block++) {
		extension = edid + block * 128U;

		/* Other extension formats are not interpreted as CTA timing arrays. */
		if (extension[0] != 2U)
			continue;

		/* Offset zero means this CTA block has no detailed timing region. */
		offset = extension[2];
		if (offset == 0U)
			continue;

		/* A DTD may neither overlap the CTA header nor include its checksum byte. */
		if (offset < 4U || offset > 127U)
			return EINVAL;

		/* Parse only complete descriptors before the extension checksum. */
		while (offset + 18U <= 127U) {
			error = display_edid_timing(output, extension + offset);
			if (error != 0)
				return error;
			offset += 18U;
		}
	}

	/* Succeeded: only verified supported progressive detailed timings have been retained. */
	return 0;
}

/* Retains one supported progressive detailed timing without fabricating a fixed mode list. */
static int
display_edid_timing(
	struct venus_display_output *output,
	const uint8_t *descriptor)
{
	struct venus_display_timing *timing;
	struct venus_display_timing **position;
	uint32_t width;
	uint32_t height;
	uint32_t horizontal_blank;
	uint32_t vertical_blank;
	uint32_t clock;
	uint64_t total;
	uint64_t refresh;
	int error;

	/* Zero pixel clock identifies a monitor-data descriptor rather than a timing. */
	clock = (uint32_t)descriptor[0] | ((uint32_t)descriptor[1] << 8);
	if (clock == 0U)
		return 0;

	/* Interlaced timings need field-level scanout semantics absent from this progressive contract. */
	if ((descriptor[17] & 128U) != 0U)
		return 0;

	/* Twelve-bit active and blanking extents define the complete horizontal timing period. */
	width = (uint32_t)descriptor[2] | ((uint32_t)(descriptor[4] & 240U) << 4);
	horizontal_blank = (uint32_t)descriptor[3] | ((uint32_t)(descriptor[4] & 15U) << 8);

	/* Vertical active and blanking extents use their independent high-bit nibbles. */
	height = (uint32_t)descriptor[5] | ((uint32_t)(descriptor[7] & 240U) << 4);
	vertical_blank = (uint32_t)descriptor[6] | ((uint32_t)(descriptor[7] & 15U) << 8);

	/* Empty blanking or undersized native images cannot form an implemented scanout timing. */
	if (width < 16U ||
	    height < 16U ||
	    horizontal_blank == 0U ||
	    vertical_blank == 0U)
		return 0;

	/* Pixel clock units are 10kHz; division rounds the actual nominal frame rate to millihertz. */
	total = (uint64_t)(width + horizontal_blank) * (height + vertical_blank);
	refresh = ((uint64_t)clock * 10000000U + total / 2U) / total;
	if (refresh < 1000U || refresh > VENUS_DISPLAY_MAX_REFRESH)
		return 0;

	/* The same geometry and cadence limits govern both discovery and later presentation. */
	error = display_validate_mode(width, height, (uint32_t)refresh);
	if (error != 0)
		return 0;

	/* Repeated DTDs in base and extension blocks describe one public mode. */
	position = &output->timings;
	while (*position != NULL) {
		/* Matching dimensions at a different frequency remain distinct modes. */
		if ((*position)->width == width &&
		    (*position)->height == height &&
		    (*position)->refresh == (uint32_t)refresh)
			return 0;

		/* Append in native EDID order so the first preferred matching DTD remains stable. */
		position = &(*position)->next;
	}

	/* Dynamic records scale with the verified EDID contents instead of a fixed synthetic mode table. */
	timing = kern_calloc(1U, sizeof(*timing));
	if (timing == NULL)
		return ENOMEM;

	/* This controller generation owns every published mode record through its topology refresh. */
	timing->width = width;
	timing->height = height;
	timing->refresh = (uint32_t)refresh;
	*position = timing;

	/* Succeeded: one complete supported nominal timing is available to discovery and validation. */
	return 0;
}

/* Frees one generation's timing inventory without touching any active scanout allocation. */
static void
display_timings_free(
	struct venus_display_output *output)
{
	struct venus_display_timing *timing;

	/* Timing descriptions own only metadata and are independent of current display-image lifetime. */
	while (output->timings != NULL) {
		timing = output->timings;
		output->timings = timing->next;
		kern_free(timing);
	}

	/* Succeeded: no obsolete EDID timing record survives this output generation. */
	return;
}
