/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Executes the production Venus display and resource code against a finite host peer.
 * Console snapshots and scheduler transitions are modeled at their public boundaries.
 */

#define main venus_backend_unused_main
#define drv_venus_transport_command console_base_transport
#include "../../ws014/tests/venus-backend.c"
#undef drv_venus_transport_command
#undef main

/* Keeps guest scheduler types distinct from the host libc signal namespace. */
typedef int32_t tid_t;
#define sigset_t console_kernel_sigset_t
#include <kern/thread.h>
#undef sigset_t

int drv_venus_transport_command(struct venus_transport *transport, const void *input, uint32_t bytes, void *output, uint32_t capacity, uint32_t *response_bytes);
#include "../../../src/drivers/gpu/venus/display.c"

/* One fake kernel thread is owned from create until its successful explicit reap. */
static struct thread console_thread;

/* The worker entry remains callable so the real polling loop can be exercised finitely. */
static void (*console_entry)(void *);

/* The current controller survives every modeled thread and transport access. */
static struct venus_controller *console_controller;

/* Scheduler ticks advance only at ordinary sleep boundaries in this deterministic fixture. */
static uint64_t console_ticks;

/* Text generation changes the independent BGRA snapshot, making redraw observable. */
static uint32_t console_generation = 1U;

/* These controls select finite thread allocation, stop and active-loop outcomes. */
static unsigned console_create_fail, console_reap_stalled, console_worker_iterations;

/* Counters observe thread creation/reaping and complete fenced display work. */
static unsigned console_created, console_reaped, console_fenced, console_snapshots;

static void console_test_restore(void);
static void console_test_timeout(void);
static void console_test_claim(struct venus_controller *controller, void *session, struct gpu_display_claim *claim);
static void console_test_finish(struct venus_controller *controller);

/*
 * Supplies complete virtual display responses while preserving the existing strict resource peer.
 */
int
drv_venus_transport_command(
	struct venus_transport *transport,
	const void *input,
	uint32_t bytes,
	void *output,
	uint32_t capacity,
	uint32_t *response_bytes)
{
	const uint8_t *command;
	uint32_t type;
	uint32_t flags;
	uint64_t fence;
	int error;

	/* Native inventory is a complete sixteen-entry protocol response with one connected output. */
	command = input;
	type = drv_venus_load32(command);
	if (type == 0x100U) {
		assert(bytes == 24U);
		assert(capacity == 408U);
		memset(output, 0, capacity);
		drv_venus_store32(output, 0x1101U);
		drv_venus_store32((uint8_t *)output + 32U, 640U);
		drv_venus_store32((uint8_t *)output + 36U, 480U);
		drv_venus_store32((uint8_t *)output + 40U, 1U);
		*response_bytes = 408U;
		return 0;
	}

	/* The independently implemented legacy peer validates transport shape and injected uncertainty. */
	error = console_base_transport(transport, input, bytes, output, capacity, response_bytes);
	if (error != 0)
		return error;

	/* Every direct display transition must use context zero and return its exact completion fence. */
	flags = drv_venus_load32(command + 4U);
	if ((flags & 1U) != 0U) {
		assert(drv_venus_load32(command + 16U) == 0U);
		fence = drv_venus_load64(command + 8U);
		assert(fence != 0U);
		drv_venus_store32((uint8_t *)output + 4U, 1U);
		drv_venus_store64((uint8_t *)output + 8U, fence);
		console_fenced++;
	}

	/* Succeeded: scanout and backing state change only after the modeled acknowledged command. */
	return 0;
}

/*
 * Reads the independent peer's complete little-endian fence quantity.
 */
uint64_t
drv_venus_load64(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;
	uint64_t low;
	uint64_t high;

	/* Separate words preserve the protocol's byte order and permit unaligned command storage. */
	bytes = buffer;
	low = drv_venus_load32(bytes);
	high = drv_venus_load32(bytes + 4U);

	/* Succeeded: the caller observes the exact two-word fence identity. */
	return low | (high << 32);
}

/*
 * Models the configuration event acknowledgment without real device access.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t word)
{
	/* This fixture owns ordinary aligned configuration storage. */
	drv_venus_store32((void *)address, word);

	/* Succeeded: subsequent configuration reads observe the written register value. */
	return;
}

/*
 * Returns a finite retained-grid snapshot whose bytes depend on text generation.
 */
int
kern_text_snapshot(
	struct kern_text_snapshot *snapshot)
{
	unsigned index;

	/* A small two-by-two text grid gives the engine a complete independently checkable image. */
	assert(snapshot != NULL);
	snapshot->width = 16U;
	snapshot->height = 32U;
	snapshot->stride = 64U;
	if (snapshot->pixels == NULL) {
		assert(snapshot->bytes == 0U);
		return 0;
	}

	/* The engine must allocate the complete source before requesting any rendered pixel. */
	assert(snapshot->bytes >= 2048U);
	for (index = 0U; index < 512U; index++)
		snapshot->pixels[index] = (console_generation << 16) | (index & 255U);
	console_snapshots++;

	/* Succeeded: changing the text generation changes real source and scanout backing bytes. */
	return 0;
}

/*
 * Samples the modeled completed text mutation counter.
 */
uint32_t
kern_text_generation(
	void)
{
	/* Succeeded: no display polling or snapshot read mutates this text generation. */
	return console_generation;
}

/*
 * Allocates a kernel-worker identity without starting it before publication.
 */
int
kthread_create(
	void (*entry)(void *),
	void *argument,
	int priority,
	struct thread **result)
{
	/* A creation failure must occur before any worker can retain its controller. */
	if (console_create_fail != 0U) {
		console_create_fail = 0U;
		return ENOMEM;
	}

	/* Exactly one controller worker is modeled at a time. */
	assert(console_created == console_reaped);
	assert(priority == SCHED_PRIORITY_DEFAULT);
	memset(&console_thread, 0, sizeof(console_thread));
	console_entry = entry;
	console_controller = argument;
	*result = &console_thread;
	console_created++;

	/* Succeeded: the caller owns an unpublished worker and its retained controller argument. */
	return 0;
}

/*
 * Observes publication before marking the modeled worker runnable.
 */
void
thread_start(
	struct thread *worker)
{
	/* The real start must follow assignment of the controller's only worker pointer. */
	assert(worker == &console_thread);
	assert(console_controller->display->console_worker == worker);
	worker->state = THREAD_RUNNING;

	/* Succeeded: later finite scheduler transitions may execute or reap this worker. */
	return;
}

/*
 * Reaps only a completed worker and consumes its creator reference once.
 */
int
thread_wait(
	struct thread *worker,
	int *status)
{
	/* Live workers cannot authorize DMA or controller retirement. */
	assert(worker == &console_thread);
	assert(status == NULL);
	if (worker->state != THREAD_ZOMBIE)
		return EBUSY;

	/* The caller owns the one successful transition from zombie to fully consumed identity. */
	worker->state = THREAD_DEAD;
	console_reaped++;

	/* Succeeded: no thread wrapper retains the controller after this return. */
	return 0;
}

/*
 * Reports deterministic monotonic kernel ticks.
 */
uint64_t
sched_ticks(
	void)
{
	/* Succeeded: only a modeled sleep advances this clock. */
	return console_ticks;
}

/*
 * Advances an ordinary sleep and models only explicit finite worker-stop events.
 */
void
sched_sleep(
	uint64_t target)
{
	/* Display refresh and worker polling must request monotonic future deadlines. */
	assert(target > console_ticks);
	console_ticks = target;

	/* A selected number of worker iterations makes the real loop return at its next stop check. */
	if (console_worker_iterations != 0U) {
		console_worker_iterations--;
		if (console_worker_iterations == 0U)
			console_controller->display->console_stopping = 1U;
	}

	/* Stop-and-reap cannot advance a deliberately stalled worker until the fixture releases it. */
	if (console_controller != NULL &&
	    console_controller->display->console_stopping != 0U &&
	    console_reap_stalled == 0U) {
		console_thread.state = THREAD_ZOMBIE;
	}

	/* Succeeded: sleep permitted progress without executing GPU commands under a console lock. */
	return;
}

/*
 * Checks native lease restoration, text redraw, preserved contexts and bounded stop ownership.
 */
int
main(
	void)
{
	/* Normal release and closing another owner both restore the same controller-owned console. */
	console_test_restore();

	/* A transport timeout or stalled worker cannot free DMA before an acknowledged reset. */
	console_test_timeout();
	assert(console_created == console_reaped);
	assert(fixture_allocations == 0U);
	assert(fixture_dma == 0U);
	assert(console_snapshots >= 4U);
	assert(console_fenced >= 12U);
	puts("Venus console: lease exclusion, live text pixels, close restore, context-zero ownership, worker stop/reap and timeout quarantine PASS");

	/* Succeeded: no modeled worker, native resource or controller allocation remains live. */
	return 0;
}

/* Claims the first native plane using its current independently discovered generation. */
static void
console_test_claim(
	struct venus_controller *controller,
	void *session,
	struct gpu_display_claim *claim)
{
	int error;

	/* Every claim uses the same public driver callback and a fresh zero output lease. */
	memset(claim, 0, sizeof(*claim));
	claim->display_id = 1U;
	claim->generation = controller->display->outputs[0].generation;
	error = display_claim(controller, session, claim);
	assert(error == 0);
	assert(claim->lease != 0U);

	/* Succeeded: the supplied session now owns one ordinary exclusive native plane. */
	return;
}

/* Exercises private console frames across release, user context teardown and later ownership changes. */
static void
console_test_restore(
	void)
{
	struct venus_controller controller;
	struct gpu_display_claim claim;
	struct gpu_display_claim foreign_claim;
	struct gpu_display_release release;
	struct gpu_display_present present;
	struct gpu_display_info inventory;
	struct gpu_present legacy;
	struct gpu_resource_create allocation;
	struct venus_resource *console_front;
	struct venus_resource *resource;
	uint8_t configuration[16];
	void *first;
	void *second;
	void *storage;
	unsigned commands;
	unsigned snapshots;
	uint32_t generation;
	int error;

	/* Creates two independent real backend contexts and one native display inventory. */
	fixture_controller(&controller, configuration);
	error = display_refresh(&controller);
	assert(error == 0);
	error = venus_open(&controller, &first);
	assert(error == 0);
	error = venus_open(&controller, &second);
	assert(error == 0);

	/* Worker allocation failure must leave the plane available and no thread reference published. */
	console_create_fail = 1U;
	memset(&claim, 0, sizeof(claim));
	claim.display_id = 1U;
	claim.generation = 1U;
	error = display_claim(&controller, first, &claim);
	assert(error == ENOMEM);
	assert(claim.lease == 0U);
	assert(controller.display->outputs[0].owner == NULL);
	assert(controller.display->console_worker == NULL);

	/* A valid native claim blocks both a foreign open and every console update. */
	console_test_claim(&controller, first, &claim);
	foreign_claim = claim;
	foreign_claim.lease = 0U;
	error = display_claim(&controller, second, &foreign_claim);
	assert(error == EBUSY);
	snapshots = console_snapshots;
	error = display_console_update(&controller);
	assert(error == 0 && console_snapshots == snapshots);

	/* Presents an actual user-owned source through the fenced private front/back path. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.bytes = 1024U;
	error = venus_resource_create(&controller, first, &allocation, &storage);
	assert(error == 0);
	memset(&present, 0, sizeof(present));
	present.lease = claim.lease;
	present.generation = claim.generation;
	present.width = 16U;
	present.height = 16U;
	present.stride = 64U;
	present.format = GPU_PIXEL_BGRA8888;
	present.refresh_millihz = VENUS_DISPLAY_REFRESH;
	present.flags = GPU_DISPLAY_PRESENT_FIFO;
	error = display_present(&controller, first, storage, &present);
	assert(error == 0 && present.sequence == 1U);
	resource = controller.display->outputs[0].front;
	assert(fixture_scanout == resource->identifier);
	assert(resource->context == ((struct venus_session *)first)->context);

	/* A foreign release cannot cause console takeover or retire the current scanout image. */
	memset(&release, 0, sizeof(release));
	release.lease = claim.lease;
	error = display_release(&controller, second, &release);
	assert(error == EPERM);
	assert(fixture_scanout == resource->identifier);

	/* Explicit release retires application-owned private display frames before restoring text. */
	error = display_release(&controller, first, &release);
	assert(error == 0);
	error = display_console_update(&controller);
	assert(error == 0);
	console_front = controller.display->console.front;
	assert(console_front != NULL && console_front->context == 0U);
	assert(fixture_scanout == console_front->identifier);
	assert(((uint32_t *)console_front->backing.address)[0] == (console_generation << 16));
	assert(controller.display->console_source->context == 0U);

	/* Public discovery describes the actual unleased console rather than an inactive user plane. */
	memset(&inventory, 0, sizeof(inventory));
	error = display_query(&controller, first, &inventory);
	assert(error == 0);
	assert(inventory.flags & GPU_DISPLAY_ACTIVE);
	assert(inventory.current_width == 16U && inventory.current_height == 32U);

	/* An unchanged console sends no redundant GPU command or repeated cell snapshot. */
	commands = console_fenced;
	snapshots = console_snapshots;
	error = display_console_update(&controller);
	assert(error == 0);
	assert(console_fenced == commands && console_snapshots == snapshots);

	/* A later shell write updates actual displayed bytes without any Vulkan or application activity. */
	console_generation++;
	error = display_console_update(&controller);
	assert(error == 0);
	console_front = controller.display->console.front;
	assert(((uint32_t *)console_front->backing.address)[0] == (console_generation << 16));
	assert(fixture_scanout == console_front->identifier);

	/* Native ownership suppresses changed text until close consumes that session's lease. */
	console_test_claim(&controller, second, &claim);
	generation = controller.display->console_generation;
	console_generation++;
	error = display_console_update(&controller);
	assert(error == 0);
	assert(controller.display->console_generation == generation);
	venus_close(&controller, second);
	error = display_console_update(&controller);
	assert(error == 0);
	assert(controller.display->console_generation == console_generation);

	/* The older copied-present API has the same exclusive precedence over controller-owned text. */
	memset(&legacy, 0, sizeof(legacy));
	legacy.width = 16U;
	legacy.height = 16U;
	legacy.stride = 64U;
	legacy.format = GPU_PIXEL_BGRA8888;
	error = venus_present(&controller, first, storage, &legacy);
	assert(error == 0);
	assert(controller.primary_scanout == storage);
	assert(controller.primary_width == 16U && controller.primary_height == 16U);
	snapshots = console_snapshots;
	error = display_console_update(&controller);
	assert(error == 0 && console_snapshots == snapshots);

	/* Discovery reports the selected legacy geometry rather than the parked console dimensions. */
	memset(&inventory, 0, sizeof(inventory));
	error = display_query(&controller, first, &inventory);
	assert(error == 0);
	assert(inventory.flags & GPU_DISPLAY_ACTIVE);
	assert(inventory.current_width == 16U && inventory.current_height == 16U);

	/* User resource destruction and both context closes cannot consume context-zero console storage. */
	venus_resource_destroy(&controller, first, storage);
	assert(controller.primary_scanout == NULL);
	venus_close(&controller, first);
	error = display_console_update(&controller);
	assert(error == 0);
	assert(controller.primary_scanout == controller.display->console.front);
	assert(controller.primary_width == 16U && controller.primary_height == 32U);
	assert(controller.display->console.front->context == 0U);
	assert(controller.resources != NULL);

	/* Runs the actual worker loop for one finite pass, then observes its ordinary stop boundary. */
	console_worker_iterations = 1U;
	console_entry(&controller);
	assert(controller.display->console_stopping != 0U);
	console_thread.state = THREAD_ZOMBIE;
	error = drv_venus_display_stop(&controller);
	assert(error == 0);
	assert(controller.display->console_worker == NULL);
	console_test_finish(&controller);

	/* Succeeded: every application and console owner retired in the intended order. */
	return;
}

/* Preserves worker and DMA ownership through uncertain hardware completion and stalled reap. */
static void
console_test_timeout(
	void)
{
	struct venus_controller controller;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	uint8_t configuration[16];
	void *session;
	unsigned buffers;
	int error;

	/* Builds console ownership independently of any long-lived application resource. */
	fixture_controller(&controller, configuration);
	error = display_refresh(&controller);
	assert(error == 0);
	error = venus_open(&controller, &session);
	assert(error == 0);
	console_test_claim(&controller, session, &claim);
	memset(&release, 0, sizeof(release));
	release.lease = claim.lease;
	error = display_release(&controller, session, &release);
	assert(error == 0);

	/* Uncertain first transfer quarantines the newly created console source and private image. */
	fixture_timeout = 0x105U;
	error = display_console_update(&controller);
	assert(error == ETIMEDOUT);
	assert(controller.transport.failed != 0U);
	buffers = fixture_dma;
	assert(buffers >= 2U);
	venus_close(&controller, session);
	assert(fixture_dma == buffers);

	/* A finite failed stop leaves the complete controller and worker reference available for retry. */
	console_reap_stalled = 1U;
	error = drv_venus_display_stop(&controller);
	assert(error == ETIMEDOUT);
	assert(controller.display->console_worker == &console_thread);
	assert(fixture_dma == buffers);

	/* Once the worker exits, reap still does not prematurely retire potentially live DMA. */
	console_reap_stalled = 0U;
	error = drv_venus_display_stop(&controller);
	assert(error == 0);
	assert(fixture_dma == buffers);
	console_test_finish(&controller);

	/* Succeeded: only the modeled acknowledged reset permits final coherent storage reclamation. */
	return;
}

/* Frees controller-owned storage only after the worker and simulated hardware have stopped. */
static void
console_test_finish(
	struct venus_controller *controller)
{
	/* The explicit worker barrier precedes the same reset-boundary cleanup as PCI detach. */
	assert(controller->display->console_worker == NULL);
	fixture_drain(controller);
	drv_venus_display_finish(controller);
	console_controller = NULL;
	assert(fixture_allocations == 0U);
	assert(fixture_dma == 0U);

	/* Succeeded: no controller state survives its stopped worker or ended device accesses. */
	return;
}
