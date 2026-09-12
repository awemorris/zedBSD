/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exclusive direct displays with private front/back images and a guest clock.
 * This clock is virtual; it does not claim synchronization to host monitor vblank.
 */

#include "internal.h"

#include <drivers/gpu.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define VENUS_DISPLAY_REFRESH		50000U
#define VENUS_DISPLAY_PERIOD		(KERN_CLOCK_HZ / 50U)
#define VENUS_PROTOCOL_SCANOUTS		16U
#define VENUS_DISPLAY_EXTENT		4096U
#define VENUS_CONSOLE_POLL_TICKS	(KERN_CLOCK_HZ / 10U)
#define VENUS_CONSOLE_STOP_TICKS	(15U * KERN_CLOCK_HZ)

/*
 * One protocol scanout, whose private buffers never escape through GPU handles.
 * The owning open remains live through each serialized callback.
 */
struct venus_display_output {
	struct venus_session *owner;
	struct venus_resource *front;
	struct venus_resource *back;
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
	uint32_t console_generation;
	unsigned console_active;
	volatile unsigned console_stopping;
};

static int display_console_start(struct venus_controller *controller);
static int display_console_update(struct venus_controller *controller);
static void display_console_worker(void *argument);
static int display_query(void *device, void *session, struct gpu_display_info *request);
static int display_mode(void *device, void *session, struct gpu_display_mode *request);
static int display_claim(void *device, void *session, struct gpu_display_claim *request);
static int display_release(void *device, void *session, const struct gpu_display_release *request);
static int display_present(void *device, void *session, void *object, struct gpu_display_present *request);
static int display_wait(void *device, void *session, struct gpu_display_wait *request);
static int display_refresh(struct venus_controller *controller);
static int display_find(struct venus_controller *controller, uint32_t identifier, uint64_t generation, struct venus_display_output **result);
static int display_find_lease(struct venus_controller *controller, struct venus_session *session, uint64_t lease, struct venus_display_output **result);
static int display_validate_mode(uint32_t width, uint32_t height, uint32_t refresh);
static int display_release_output(struct venus_controller *controller, struct venus_display_output *output);
static int display_prepare(struct venus_controller *controller, struct venus_display_output *output, const struct gpu_display_present *request);
static int display_frame(struct venus_controller *controller, struct venus_display_output *output, struct venus_resource *source, struct gpu_display_present *request);
static int display_fenced(struct venus_controller *controller, uint8_t *command, uint32_t bytes);
static int display_next_refresh(struct venus_display_output *output);
static int display_status(struct venus_controller *controller, const void *command, uint32_t command_bytes, void *reply, uint32_t reply_bytes, uint32_t expected);

/* Every callback shares the controller's sleepable device mutex. */
const struct drv_gpu_display_ops drv_venus_display_operations = {
	display_query, display_mode, display_claim,
	display_release, display_present, display_wait
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
			controller->transport.failed = 1U;
			kern_logf("venus: display %u retained for reset: %d\n", output->identifier, error);
		}

		/* A retired wrapper must never remain in lease arbitration. */
		output->owner = NULL;
		output->lease = 0U;
	}

	/* Succeeded: no display record refers to the closing session. */
	return;
}

/*
 * Frees metadata after transport reset has already retired every resource.
 */
void
drv_venus_display_finish(
	struct venus_controller *controller)
{
	/* Controller detach calls this only after checked device reset. */
	if (controller->display == NULL)
		return;

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
	memset(&snapshot, 0, sizeof(snapshot));
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
	struct venus_display_engine *engine;
	struct kern_text_snapshot snapshot;
	struct gpu_display_present request;
	uint64_t bytes;
	uint32_t generation;
	int error;

	/* Transport failure leaves all display backing quarantined for the existing reset owner. */
	if (controller->transport.failed != 0U)
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
	memset(&snapshot, 0, sizeof(snapshot));
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
	memset(&request, 0, sizeof(request));
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
	uint64_t now;
	unsigned stopping;
	int previous_error;
	int error;

	/* The PCI controller outlives this worker through its explicit stop-and-reap barrier. */
	controller = argument;
	previous_error = 0;
	while (1) {
		/* Stop is independent from the controller mutex so detach can request it during a command wait. */
		stopping = __atomic_load_n(&controller->display->console_stopping, __ATOMIC_ACQUIRE);
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

		/* Polling uses a sleepable bounded interval and sends no GPU traffic for unchanged text. */
		now = sched_ticks();
		if (now > UINT64_MAX - VENUS_CONSOLE_POLL_TICKS)
			break;

		/* Console mutations remain observable through the generation sampled after this finite wait. */
		sched_sleep(now + VENUS_CONSOLE_POLL_TICKS);
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
	    GPU_DISPLAY_ATOMIC_MODE_PRESENT;
	if (output->connected != 0U)
		request->flags |= GPU_DISPLAY_CONNECTED;
	if (output->front != NULL)
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
	request->refresh_millihz = VENUS_DISPLAY_REFRESH;
	snprintf(request->name, sizeof(request->name), "Venus virtual display %u", request->index);
	mutex_unlock(&controller->mutex);

	/* Succeeded: the caller has a bounded, generation-tagged display snapshot. */
	return 0;
}

/* Enumerates the preferred mode or validates another supported virtual extent. */
static int
display_mode(
	void *device,
	void *private_session,
	struct gpu_display_mode *request)
{
	struct venus_controller *controller;
	struct venus_display_output *output;
	int error;

	/* Mode descriptions do not acquire presentation ownership. */
	(void)private_session;
	controller = device;
	mutex_lock(&controller->mutex);
	error = display_find(controller, request->display_id, request->generation, &output);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* The host-preferred rectangle is the one advertised enumerated mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY) {
			mutex_unlock(&controller->mutex);
			return 0;
		}
		if (request->index != 0U) {
			mutex_unlock(&controller->mutex);
			return EINVAL;
		}
		request->width = output->preferred_width;
		request->height = output->preferred_height;
		request->refresh_millihz = VENUS_DISPLAY_REFRESH;
	} else {
		/* Custom virtual modes must preserve the documented bounded clock. */
		error = display_validate_mode(request->width, request->height, request->refresh_millihz);
		if (error != 0) {
			mutex_unlock(&controller->mutex);
			return error;
		}
	}
	mutex_unlock(&controller->mutex);

	/* Succeeded: neither enumeration nor validation changed the display mode. */
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
		return ENODEV;
	}

	/* Private storage and session ownership are independently checked in K. */
	if (source->controller != controller ||
	    source->context != output->owner->context ||
	    source->kind != VENUS_RESOURCE_STORAGE) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}

	/* A completed call has consumed its source and no longer borrows userspace. */
	error = display_frame(controller, output, source, request);
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

	/* Succeeded: all earlier sequences have finished reading their source pixels. */
	return 0;
}

/* Refreshes native topology only when first queried or explicitly invalidated. */
static int
display_refresh(
	struct venus_controller *controller)
{
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
	if (controller->transport.failed != 0U)
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

	/* Acknowledges the observed event before querying, preserving later new events. */
	if ((events & 1U) != 0U)
		kern_mmio_write32(configuration + 4U, 1U);
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
		return ENODEV;

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
	struct venus_display_output *output;
	uint32_t index;

	/* A missing engine or reserved lease zero cannot describe ownership. */
	if (controller->display == NULL || lease == 0U)
		return EINVAL;
	if (controller->transport.failed != 0U)
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

/* Checks a virtual mode without changing scanout or allocating display buffers. */
static int
display_validate_mode(
	uint32_t width,
	uint32_t height,
	uint32_t refresh)
{
	uint64_t bytes;

	/* The fixed virtual refresh period is exactly representable by kernel ticks. */
	if (refresh != VENUS_DISPLAY_REFRESH)
		return EINVAL;
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
	if (output->front != NULL) {
		memset(command, 0, sizeof(command));
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
	if (output->identifier == 1U)
		controller->display->console_active = 0U;

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
	memset(&geometry, 0, sizeof(geometry));
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
		memcpy(
			destination + (size_t)row * request->width * 4U,
			pixels + (size_t)row * request->stride,
			(size_t)request->width * 4U);
	}

	/* A transport receipt alone cannot prove the GL transfer has completed. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0105U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store32(command + 48U, output->back->identifier);
	kern_io_write_barrier();
	error = display_fenced(controller, command, sizeof(command));
	if (error != 0)
		return error;

	/* The queue consumes at most one completed image per virtual refresh tick. */
	error = display_next_refresh(output);
	if (error != 0)
		return error;

	/* Geometry and the fully transferred image change in the same scanout command. */
	memset(command, 0, 48U);
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

	/* Completes the whole visible update before reporting a reusable source image. */
	memset(command, 0, 48U);
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
		controller->transport.failed = 1U;
		return EIO;
	}

	/* A completed fence must name this exact command rather than an earlier display update. */
	returned_fence = drv_venus_load64(response + 8U);
	if (returned_fence != fence) {
		controller->transport.failed = 1U;
		return EIO;
	}

	/* Succeeded: the host finished this display command before its reply. */
	return 0;
}

/* Sleeps to the next guest virtual refresh without masking timer interrupts. */
static int
display_next_refresh(
	struct venus_display_output *output)
{
	uint64_t now;
	uint64_t target;

	/* Refresh boundaries are fixed in the monotonic kernel tick domain. */
	now = sched_ticks();
	if (now > UINT64_MAX - VENUS_DISPLAY_PERIOD)
		return EOVERFLOW;
	target = now - now % VENUS_DISPLAY_PERIOD + VENUS_DISPLAY_PERIOD;

	/* Serialized completed presents cannot reuse this strictly future clock boundary. */
	if (now < output->present_tick)
		return EIO;

	/* Early wakeups recheck the same finite absolute deadline. */
	while (now < target) {
		sched_sleep(target);
		now = sched_ticks();
	}

	/* Succeeded: this completed frame may become the current virtual display image. */
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
		controller->transport.failed = 1U;
		return EIO;
	}

	/* Interprets the response tag only after its complete required interval was received. */
	response_type = drv_venus_load32(reply);
	if (response_type != expected) {
		controller->transport.failed = 1U;
		return EIO;
	}

	/* Succeeded: the caller may inspect the complete expected response. */
	return 0;
}
