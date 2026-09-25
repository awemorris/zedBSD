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

#define mutex_lock console_base_mutex_lock
#define mutex_unlock console_base_mutex_unlock
#define main venus_backend_unused_main
#define drv_venus_transport_command console_base_transport
#include "../ws014/tests/venus-backend.c"
#undef drv_venus_transport_command
#undef main
#undef mutex_lock
#undef mutex_unlock

/* Keeps guest scheduler types distinct from the host libc signal namespace. */
typedef int32_t tid_t;
#define sigset_t console_kernel_sigset_t
#include <kern/thread.h>
#undef sigset_t

void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);

int drv_venus_transport_command(struct venus_transport *transport, const void *input, uint32_t bytes, void *output, uint32_t capacity, uint32_t *response_bytes);
#include "../../src/drivers/gpu/venus/display.c"

/* One fake kernel thread is owned from create until its successful explicit reap. */
static struct thread console_thread;

/* The worker entry remains callable so the real notification loop can be exercised finitely. */
static void (*console_entry)(void *);

/* The current controller survives every modeled thread and transport access. */
static struct venus_controller *console_controller;

/* Scheduler ticks advance only at ordinary sleep boundaries in this deterministic fixture. */
static uint64_t console_ticks;

/* Optional observation isolates the real display callback's controller lock across its sleep boundary. */
static struct mutex *console_observed_mutex;
/* Exactly one observed controller mutex hold may surround each native display callback. */
static unsigned console_observed_depth;

/* A serial host peer observes the driver's short counter-snapshot spin sections. */
static struct spinlock *console_observed_spin;
static struct venus_display_output *console_race_output;

/* Text generation changes the independent BGRA snapshot, making redraw observable. */
static uint32_t console_generation = 1U;

/* The text peer owns no callback, only the currently armed condition destination. */
static struct kern_text_observer *console_subscription;

/* These counters distinguish genuine change notifications from periodic worker wakeups. */
static unsigned console_idle_waits;

/* Changed condition sequences count sleep refusals instead of lost text updates. */
static unsigned console_early_wakes;

/* One mutation during rasterization exercises the worker's observe-before-render handshake. */
static unsigned console_snapshot_change;

/* These controls select finite thread allocation, stop and active-loop outcomes. */
static unsigned console_create_fail, console_reap_stalled, console_worker_iterations;

/* Counters observe thread creation/reaping and complete fenced display work. */
static unsigned console_created, console_reaped, console_fenced, console_snapshots;

/* Optional EDID bytes are supplied only by the native timing fixture after feature negotiation. */
static uint8_t console_edid[1024];

/* The native peer reports this independent bounded EDID payload length. */
static uint32_t console_edid_bytes;


static void console_test_restore(void);
static void console_test_timeout(void);
static void console_test_claim(struct venus_controller *controller, void *session, struct gpu_display_claim *claim);
static void console_test_finish(struct venus_controller *controller);
static void console_text_changed(void);

/*
 * Records configuration invalidation in the serial display peer's retained transport.
 */
void
drv_venus_transport_display_changed(
	struct venus_transport *transport)
{
	/* This peer has no IRQ dispatcher; real interrupt publication has its own fixture. */
	transport->topology_sequence++;

	/* Succeeded: subsequent display-event snapshots can observe the invalidation. */
	return;
}

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

	/* Negotiated EDID requests must use native scanout zero and the full fixed virtio response. */
	command = input;
	type = drv_venus_load32(command);
	if (type == 0x10aU) {
		assert((transport->features & VENUS_FEATURE_EDID) != 0U);
		assert(bytes == 32U);
		assert(capacity == 1056U);
		assert(drv_venus_load32(command + 16U) == 0U);
		assert(drv_venus_load32(command + 24U) == 0U);
		assert(drv_venus_load32(command + 28U) == 0U);
		assert(console_edid_bytes <= sizeof(console_edid));
		fixture_commands[type]++;
		memset(output, 0, capacity);
		drv_venus_store32(output, 0x1104U);
		drv_venus_store32((uint8_t *)output + 24U, console_edid_bytes);
		memcpy((uint8_t *)output + 32U, console_edid, console_edid_bytes);
		*response_bytes = capacity;
		return 0;
	}

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

	/* A writer can update retained text after the worker sampled its target generation. */
	if (console_snapshot_change != 0U) {
		console_snapshot_change = 0U;
		console_text_changed();
	}

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
	/* A nominal refresh wait cannot retain controller admission against another rendering session. */
	if (console_observed_mutex != NULL)
		assert(console_observed_depth == 0U);

	/* A competing owner may retire or replace the lease while the pacing helper is asleep. */
	if (console_race_output != NULL) {
		console_race_output->lease++;
		console_race_output = NULL;
	}

	/* Display refresh and worker polling must request monotonic future deadlines. */
	assert(target > console_ticks);
	console_ticks = target;

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
#ifndef VENUS_CONSOLE_ENTRY
#define VENUS_CONSOLE_ENTRY main
#endif
int
VENUS_CONSOLE_ENTRY(
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

/*
 * Initializes a finite console notification condition without changing transport synchronization.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* No observed interrupt-side section may span construction of a new condition. */
	assert(console_observed_spin == NULL);
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;

	/* Succeeded: the production worker may now share this leaf lock with notifications. */
	return;
}

/*
 * Establishes the first change sequence for the modeled console condition.
 */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* The fixture has no live scheduler tokens, but retains the actual sequence handshake. */
	memset(queue, 0, sizeof(*queue));
	queue->sequence = 1U;
	queue->name = name;

	/* Succeeded: later mutation and ownership notifications advance from a known baseline. */
	return;
}

/*
 * Observes the notification sequence at the worker's real condition boundary.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* Snapshotting a condition must retain its leaf lock until the observation is complete. */
	assert(console_observed_spin != NULL);

	/* Succeeded: the production worker will compare this observation before registering sleep. */
	return queue->sequence;
}

/*
 * Publishes an ownership, stop or text change under the shared leaf lock.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* Unlocked wakeups could race actual kernel sleep registration even in a serial test. */
	assert(console_observed_spin != NULL);
	queue->sequence++;

	/* Succeeded: a worker holding an earlier observation cannot sleep through this change. */
	return;
}

/*
 * Models a bounded test action at the real worker's indefinite condition sleep.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	/* Every idle console wait must release controller admission and carry no periodic deadline. */
	assert(console_observed_spin == lock);
	assert(console_observed_depth == 0U);
	assert(deadline == 0U && flags == 0U);

	/* A notification during snapshot/rendering must reject sleep instead of being lost. */
	if (queue->sequence != observed) {
		console_early_wakes++;
		return EAGAIN;
	}

	/* The finite test requests exit instead of leaving its modeled kernel worker asleep forever. */
	console_idle_waits++;
	assert(console_worker_iterations != 0U);
	console_worker_iterations--;
	if (console_worker_iterations == 0U)
		console_controller->display->console_stopping = 1U;

	/* Succeeded: the next production-loop iteration observes the explicit stop predicate. */
	return 0;
}

/*
 * Publishes the production console's condition to the modeled text mutation source.
 */
int
kern_text_observe(
	struct kern_text_observer *observer,
	struct spinlock *lock,
	struct wait_queue *queue)
{
	/* Registration must never invert the leaf condition lock and text registry ordering. */
	assert(console_observed_spin == NULL);
	assert(console_subscription == NULL);
	assert(lock->rank == LOCK_RANK_POLL);
	observer->lock = lock;
	observer->queue = queue;
	console_subscription = observer;

	/* Succeeded: only subsequent text mutations may wake this active console. */
	return 0;
}

/*
 * Ends text notification before primary ownership or controller retirement.
 */
void
kern_text_unobserve(
	struct kern_text_observer *observer)
{
	/* Removing a subscription while holding its destination lock would deadlock a real writer. */
	assert(console_observed_spin == NULL);
	if (console_subscription == observer)
		console_subscription = NULL;

	/* Succeeded: the serial text peer can no longer reference the caller-owned condition. */
	return;
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
	assert(console_subscription == NULL);
	assert(controller.display->console_observing == 0U);

	/* A parked claimed console has no periodic timer and receives no text wakeups. */
	console_worker_iterations = 1U;
	console_entry(&controller);
	assert(console_idle_waits == 1U);
	controller.display->console_stopping = 0U;
	console_text_changed();
	assert(console_subscription == NULL);

	/* Another open still cannot bypass the primary owner's display lease. */
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
	assert(console_subscription == &controller.display->console_observer);
	assert(controller.display->console_observing == 1U);

	/* The newly armed worker restores retained text even without another mutation. */
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
	console_text_changed();
	error = display_console_update(&controller);
	assert(error == 0);
	console_front = controller.display->console.front;
	assert(((uint32_t *)console_front->backing.address)[0] == (console_generation << 16));
	assert(fixture_scanout == console_front->identifier);

	/* Native ownership suppresses changed text until close consumes that session's lease. */
	console_test_claim(&controller, second, &claim);
	generation = controller.display->console_generation;
	console_text_changed();
	assert(console_subscription == NULL);
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

	/* A change inside rendering must force another pass before the worker may sleep. */
	console_text_changed();
	console_snapshot_change = 1U;
	console_worker_iterations = 1U;
	console_entry(&controller);
	assert(console_early_wakes != 0U);
	assert(controller.display->console_generation == console_generation);
	assert(controller.display->console_stopping != 0U);
	console_thread.state = THREAD_ZOMBIE;
	error = drv_venus_display_stop(&controller);
	assert(error == 0);
	assert(controller.display->console_worker == NULL);
	assert(console_subscription == NULL);
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
	assert(console_subscription == NULL);
	fixture_drain(controller);
	drv_venus_display_finish(controller);
	console_controller = NULL;
	assert(fixture_allocations == 0U);
	assert(fixture_dma == 0U);

	/* Succeeded: no controller state survives its stopped worker or ended device accesses. */
	return;
}

/* Observes only a selected display controller's admission without changing unrelated fixture peers. */
void
mutex_lock(
	struct mutex *mutex)
{
	if (mutex == console_observed_mutex) {
		assert(console_observed_depth == 0U);
		console_observed_depth++;
	}
	console_base_mutex_lock(mutex);
	return;
}

/* The selected callback must return with precisely the lock ownership it had on entry. */
void
mutex_unlock(
	struct mutex *mutex)
{
	if (mutex == console_observed_mutex) {
		assert(console_observed_depth == 1U);
		console_observed_depth--;
	}
	console_base_mutex_unlock(mutex);
	return;
}

/* Pacing tests deliberately retain failed transport state instead of inventing recovery authorization. */
int
drv_gpu_recovery_ready(
	struct drv_gpu_device *device)
{
	(void)device;
	return 0;
}

/* This peer exercises ordinary display callbacks, not recovery or device-wide GPU core notifications. */
void
drv_gpu_report_error(
	struct drv_gpu_device *device,
	int error)
{
	(void)device;
	assert(error == 0);
	return;
}

/* Display cadence does not submit asynchronous rendering commands in this native-only fixture. */
int
drv_venus_transport_submit(
	struct venus_transport *transport,
	uint32_t context,
	const void *command,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	(void)transport;
	(void)context;
	(void)command;
	(void)bytes;
	(void)flags;
	(void)timeline;
	(void)completion;
	assert(0);
	return EIO;
}

/* Ordinary display/resource opens have no asynchronous render completion to drain here. */
void
drv_venus_transport_drain(
	struct venus_transport *transport,
	uint32_t context)
{
	(void)transport;
	(void)context;
	return;
}

/*
 * Models a serial interrupt-masked counter snapshot without inventing concurrent kernel execution.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* A nested spin hold would expose unexpected blocking or lock recursion in this finite peer. */
	assert(lock != NULL);
	assert(console_observed_spin == NULL);
	console_observed_spin = lock;

	/* Succeeded: the matching restore must consume this exact observed counter lock. */
	return 1UL;
}

/*
 * Restores the independently observed interrupt state after the driver's counter snapshot.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* No unlock may target another lock or discard the caller's captured interrupt state. */
	assert(console_observed_spin == lock);
	assert(enabled == 1UL);
	console_observed_spin = NULL;

	/* Succeeded: no counter snapshot remains active across a later sleep or resource retirement. */
	return;
}

/* Notifies changed retained text only while the console is an active snapshot consumer. */
static void
console_text_changed(
	void)
{
	unsigned long irq;

	/* Graphics ownership still retains text generations without scheduling the console worker. */
	console_generation++;
	if (console_subscription == NULL)
		return;

	/* The actual text notification fixture separately checks registry and concurrent unlink lifetime. */
	irq = spin_lock_irqsave(console_subscription->lock);

	waitq_wake_all(console_subscription->queue);

	spin_unlock_irqrestore(console_subscription->lock, irq);

	/* Succeeded: the worker's next sequence comparison observes this completed text update. */
	return;
}
