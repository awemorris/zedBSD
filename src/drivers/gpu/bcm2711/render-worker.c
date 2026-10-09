/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Bounded trusted jobs share one worker; callback retirement never substitutes for DMA stop. */
#include <kern/kcrt.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/render-worker.h"

/* Free slots acquire a callback, execute once and finish that callback before reuse. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#define REQUEST_FREE 0U
#define REQUEST_QUEUED 1U
#define REQUEST_ACTIVE 2U
#define REQUEST_FINISHING 3U
#define REQUEST_RESERVED 4U
#define REQUEST_RETAINED 5U

/* Constant operation-table entries need declarations before ANSI C initialization. */
static int job_reserve(void *opaque, void *private_session, uint32_t timeline, struct drv_gpu_completion *completion, void **token);
static int job_commit(void *opaque, void *private_session, void *token, struct drv_gpu_completion *completion);
static int job_cancel(void *opaque, void *private_session, void *token, struct drv_gpu_completion *completion, unsigned fault);
static int job_capacity(void *opaque, void *private_session, uint32_t timeline, unsigned *available);

/* The immutable supervised-job table is bound only when the Vulkan command table is complete. */
static const struct drv_gpu_job_ops job_operations = {
	job_reserve,
	job_commit,
	job_cancel,
	job_capacity
};

static struct bcm2711_render_request *find_reservation(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, void *token, struct drv_gpu_completion *completion);
static void queue_marker(struct bcm2711_render_device *controller, struct bcm2711_render_request *request, int error);
static int execute_marker(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, void *payload, bool *retired);
static int dispose_marker(struct bcm2711_render_device *controller, void *payload, bool retired);
static unsigned reservation_capacity(struct bcm2711_render_worker *worker);

static void worker_main(void *opaque);
static struct bcm2711_render_request *take_request(struct bcm2711_render_device *controller);
static void finish_request(struct bcm2711_render_device *controller, struct bcm2711_render_request *request, int error);

/*
 * Binds the complete supervised-job table before native Vulkan capability publication.
 */
void
bcm2711_render_jobs_bind(
	struct drv_gpu_ops *operations)
{
	/* The caller publishes JOB only together with the command notification and drain contract. */
	operations->jobs = &job_operations;
}

/*
 * Initializes the private ordered queue before the renderer node publishes opens.
 */
void
bcm2711_render_worker_init(
	struct bcm2711_render_device *controller)
{
	/* No producer exists yet, so zero slots and queues can be initialized without a guard. */
	kern_memset(&controller->worker, 0, sizeof(controller->worker));
	waitq_init(&controller->worker.available, "bcm2711-render-work");
	waitq_init(&controller->worker.retired, "bcm2711-render-retired");
}

/*
 * Starts the permanent worker on the first userspace open while the controller mutex is held.
 */
int
bcm2711_render_worker_start(
	struct bcm2711_render_device *controller)
{
	struct thread *thread;
	int error;

	/* Every later open reuses the existing worker and its permanent queue owner. */
	if (controller->worker.thread != NULL)
		return 0;
	error = kthread_create(worker_main, controller, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* A complete thread owner exists before it can observe newly admitted queue producers. */
	controller->worker.thread = thread;
	thread_start(thread);

	/* Succeeded: one permanent kernel thread serves this controller's trusted native jobs. */
	return 0;
}

/*
 * Accepts one fully prepared payload and callback without allocating or touching native registers.
 * A refusal retains neither the payload nor completion; success consumes both exactly once.
 */
int
bcm2711_render_worker_submit(
	struct bcm2711_render_session *session,
	bcm2711_render_execute execute,
	bcm2711_render_dispose dispose,
	void *payload,
	struct drv_gpu_completion *completion)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_worker *worker;
	struct bcm2711_render_request *request;
	unsigned long enabled;
	uint32_t slot;

	/* Payload ownership needs both its executable operation and terminal disposal. */
	if (execute == NULL || dispose == NULL || completion == NULL)
		return EINVAL;
	controller = session->device;
	worker = &controller->worker;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	if (session->stopping || worker->uncertain ||
	    !controller->space.native->hardware.ready || controller->space.native->hardware.faulted) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EIO;
	}

	/* A fixed bounded slot pool prevents commit-time allocation and unbounded queued ownership. */
	request = NULL;
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		if (worker->requests[slot].state != REQUEST_FREE)
			continue;
		request = &worker->requests[slot];
		break;
	}

	/* Capacity refusal precedes every callback or payload ownership transfer. */
	if (request == NULL || session->pending == UINT32_MAX) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EAGAIN;
	}

	/* A fully owned callback and immutable payload enter the FIFO together. */
	request->session = session;
	request->completion = completion;
	request->execute = execute;
	request->dispose = dispose;
	request->payload = payload;
	request->state = REQUEST_QUEUED;
	request->canceled = 0;
	request->next = NULL;
	session->pending++;
	if (worker->tail == NULL) {
		worker->head = request;
	} else {
		worker->tail->next = request;
	}

	/* The tail always names the final fully owned queued request. */
	worker->tail = request;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* The permanent worker observes publication before its condition sequence changes. */
	waitq_wake_one(&worker->available);

	/* Succeeded: exactly one queued slot owns this payload and common completion. */
	return 0;
}

/*
 * Executes one queued payload through the same bounded path used by the permanent worker.
 */
int
bcm2711_render_worker_step(
	struct bcm2711_render_device *controller)
{
	struct bcm2711_render_request *request;
	unsigned long enabled;
	bool retired;
	bool global_failure;
	bool native_fault;
	int error;
	int disposed;

	/* Dequeue retains the session and callback until final completion delivery ends. */
	request = take_request(controller);
	if (request == NULL)
		return EAGAIN;
	mutex_lock(&controller->mutex);

	/* Stop and global fault may arrive after dequeue while this worker waits for the controller. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	error = request->canceled;
	if (error == 0 && request->session->stopping)
		error = ECANCELED;
	if (error == 0 && controller->space.native->hardware.faulted)
		error = EIO;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Only the trusted executor may launch work; cancellation owns no native DMA. */
	retired = true;
	if (error == 0)
		error = request->execute(controller, request->session, request->payload, &retired);
	/* An IRQ fault between queued jobs closes the common device even without a launched payload. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	native_fault = controller->space.native->hardware.faulted;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Global fault publication follows native admission closure before callback retirement. */
	global_failure = false;
	if (native_fault) {
		if (error == 0)
			error = EIO;
		bcm2711_render_worker_fault(controller, error);
		global_failure = true;
	}

	/* A launched operation without retirement proof preserves every independent native view. */
	if (!retired) {
		/* Native uncertainty arms resource teardown before payload disposal drops any job view. */
		if (error == 0)
			error = EIO;
		bcm2711_render_worker_fault(controller, error);
		global_failure = true;
	}

	/* Disposal releases independent job views; their owner quarantines uncertain native storage. */
	disposed = request->dispose(controller, request->payload, retired);
	if (disposed != 0) {
		/* A failed translation retirement also requires checked global recovery before reuse. */
		bcm2711_render_worker_fault(controller, disposed);
		global_failure = true;
		if (error == 0)
			error = disposed;
	}

	mutex_unlock(&controller->mutex);

	/* Common observers can see loss only after independent native storage is quarantined. */
	if (global_failure)
		bcm2711_render_fail(controller, error);

	/* Completion delivery runs outside controller and IRQ locks, before slot reuse or close. */
	finish_request(controller, request, error);

	/* Succeeded: one accepted payload and callback finished, including an error outcome. */
	return 0;
}

/*
 * Stops new publication for one session and marks queued payloads canceled without waiting.
 */
void
bcm2711_render_worker_stop(
	struct bcm2711_render_session *session,
	int error)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_request *request;
	unsigned long enabled;
	uint32_t slot;

	/* Stop admission and queue cancellation share the same IRQ guard as submission. */
	controller = session->device;
	if (error == 0)
		error = ECANCELED;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	session->stopping = true;

	/* An active bounded operation retains its callback until the worker's actual retirement result. */
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		request = &controller->worker.requests[slot];
		if (request->session != session)
			continue;

		/* Unpublished ordinary reservations remain withdrawable by the common stop monitor. */
		if (request->state == REQUEST_RETAINED)
			queue_marker(controller, request, error);
		if (request->state == REQUEST_QUEUED)
			request->canceled = error;
	}

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Queued canceled payloads still require disposal and exactly one common completion. */
	waitq_wake_one(&controller->worker.available);
}

/*
 * Confirms session callback and native retirement without blocking on an active worker.
 */
int
bcm2711_render_worker_stopped(
	struct bcm2711_render_session *session)
{
	struct bcm2711_render_device *controller;
	unsigned long enabled;
	uint32_t slot;
	uint32_t unpublished;
	int error;

	/* Logical callback drain and uncertain native DMA are distinct stopping conditions. */
	controller = session->device;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	/* A reserved but unpublished callback supplies no native work and remains normally cancelable. */
	unpublished = 0;
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		if (controller->worker.requests[slot].session == session &&
		    controller->worker.requests[slot].state == REQUEST_RESERVED)
			unpublished++;
	}

	/* Every posted, retained or finishing callback must end before a local stop is confirmed. */
	error = 0;
	if (!session->stopping || session->pending != unpublished)
		error = EAGAIN;
	if (error == 0 && controller->worker.uncertain)
		error = EIO;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Neither pending callbacks nor uncertain DMA supplies a safe local stop. */
	if (error != 0)
		return error;

	/* Succeeded: no posted callback or uncertain native work remains; unpublished reservations are withdrawable. */
	return 0;
}

/*
 * Joins every accepted callback before common final close destroys its session descriptor.
 * Quarantined native VA allocations persist independently of this logical drain.
 */
void
bcm2711_render_worker_drain(
	struct bcm2711_render_session *session)
{
	struct bcm2711_render_device *controller;
	unsigned long enabled;
	uint64_t observed;
	uint32_t slot;
	struct bcm2711_render_request *request;
	int error;

	/* Closing producers cannot add another callback while queued payloads retire. */
	controller = session->device;
	bcm2711_render_worker_stop(session, ECANCELED);
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	/* Final close joins unpublished callbacks that the common monitor has not withdrawn. */
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		request = &controller->worker.requests[slot];
		if (request->session == session && request->state == REQUEST_RESERVED)
			queue_marker(controller, request, ECANCELED);
	}

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Published close markers wake the permanent worker before drain observes its final condition. */
	waitq_wake_one(&controller->worker.available);
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	/* Worker completion and the wait condition share this guard, preventing a lost final wake. */
	while (session->pending != 0) {
		observed = waitq_sequence(&controller->worker.retired);
		error = waitq_sleep(&controller->worker.retired, &controller->space.native->hardware.guard, observed, 0, 0);
		if (error != 0 && error != EAGAIN)
			__builtin_trap();
	}

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
}

/*
 * Arms independent storage quarantine and closes all publication before global loss is observed.
 */
void
bcm2711_render_worker_fault(
	struct bcm2711_render_device *controller,
	int error)
{
	struct bcm2711_render_request *request;
	unsigned long enabled;
	uint32_t slot;

	/* Native faults retain all VA storage before ordinary teardown can see the common error. */
	if (error == 0)
		error = EIO;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	controller->space.native->hardware.faulted = true;
	controller->worker.uncertain = true;

	/* Accepted queued callbacks still retire exactly once, without another native launch. */
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		request = &controller->worker.requests[slot];
		if (request->state == REQUEST_QUEUED)
			request->canceled = error;
	}

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* The permanent worker disposes canceled payloads without claiming that native DMA stopped. */
	waitq_wake_one(&controller->worker.available);
}

/*
 * Clears uncertainty only after the caller verifies reset, mappings and all old owners retired.
 */
int
bcm2711_render_worker_recovered(
	struct bcm2711_render_device *controller)
{
	unsigned long enabled;
	uint32_t slot;

	/* The controller mutex excludes new producer namespaces during checked global recovery. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	/* Callback delivery must have ended before reset can reopen any native publication. */
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		if (controller->worker.requests[slot].state != REQUEST_FREE) {
			spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
			return EBUSY;
		}
	}

	/* The native reset's readbacks, cache and translation flushes supply the actual retirement proof. */
	if (!controller->space.native->hardware.ready || controller->space.native->hardware.faulted) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EIO;
	}

	/* Only checked native reinitialization clears the queue's DMA uncertainty. */
	controller->worker.uncertain = false;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: the next session may publish native work into the recovered engine. */
	return 0;
}

/* Reserves one unpublished marker and its original common callback in an owned Vulkan domain. */
static int
job_reserve(
	void *opaque,
	void *private_session,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **token)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	struct bcm2711_render_request *request;
	unsigned long enabled;
	unsigned capacity;
	uint64_t mask;
	uint32_t slot;

	/* A refused reservation transfers no callback or output token to the backend. */
	if (completion == NULL || token == NULL || timeline == 0 || timeline >= 64)
		return EINVAL;
	*token = NULL;
	controller = opaque;
	session = private_session;
	mask = UINT64_C(1) << timeline;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	/* Domain ownership comes from the actual live Vulkan queue, never an arbitrary userspace index. */
	if ((session->timelines & mask) == 0) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EINVAL;
	}

	/* Faulted or stopping namespaces cannot acquire another producer-owned callback. */
	if (session->stopping || controller->worker.uncertain ||
	    !controller->space.native->hardware.ready || controller->space.native->hardware.faulted) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EIO;
	}

	/* At most half the fixed slots are supervised markers, leaving room for actual decoder commands. */
	capacity = reservation_capacity(&controller->worker);
	if (capacity == 0 || session->pending == UINT32_MAX) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EAGAIN;
	}

	/* The capacity snapshot and slot acquisition share this guard, so a free slot must exist. */
	request = NULL;
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		if (controller->worker.requests[slot].state == REQUEST_FREE) {
			request = &controller->worker.requests[slot];
			break;
		}
	}

	/* A slot preserves the callback without entering the FIFO or touching native registers. */
	if (request == NULL)
		__builtin_trap();
	request->session = session;
	request->completion = completion;
	request->supervised = true;
	request->timeline = timeline;
	request->state = REQUEST_RESERVED;
	session->pending++;
	*token = request;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: the producer owns one bounded marker until exact commit or cancel. */
	return 0;
}

/* Commits already reserved storage as a FIFO marker after the producer's accepted native commands. */
static int
job_commit(
	void *opaque,
	void *private_session,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	struct bcm2711_render_request *request;
	unsigned long enabled;

	/* The opaque token is compared against live slots before any dereference. */
	controller = opaque;
	session = private_session;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	request = find_reservation(controller, session, token, completion);
	if (request == NULL || request->state != REQUEST_RESERVED) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return ESTALE;
	}

	/* Stop closes publication while keeping the original reservation withdrawable. */
	if (session->stopping || controller->worker.uncertain ||
	    !controller->space.native->hardware.ready || controller->space.native->hardware.faulted) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EIO;
	}

	/* The one native worker proves preceding command retirement before completing this marker. */
	queue_marker(controller, request, 0);

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Commit publishes using only reserved slot storage, with no allocation or native MMIO. */
	waitq_wake_one(&controller->worker.available);

	/* Succeeded: the original callback now belongs to an ordered completion marker. */
	return 0;
}

/* Withdraws definite nonacceptance without a callback, or retains an uncertain callback until stop. */
static int
job_cancel(
	void *opaque,
	void *private_session,
	void *token,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	struct bcm2711_render_request *request;
	unsigned long enabled;

	/* Only normal withdrawal and uncertain-work retention have defined callback ownership. */
	if (fault > 1)
		return EINVAL;
	controller = opaque;
	session = private_session;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	request = find_reservation(controller, session, token, completion);
	if (request == NULL) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return ESTALE;
	}

	/* An uncertain marker keeps its callback for stop/drain; active and finishing callbacks remain owned. */
	if (fault != 0) {
		if (request->state == REQUEST_RESERVED) {
			request->state = REQUEST_RETAINED;
			if (session->stopping)
				queue_marker(controller, request, EIO);
		}

		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		waitq_wake_one(&controller->worker.available);
		return 0;
	}

	/* Normal cancellation is valid only before marker publication, even during a proven stop. */
	if (request->state != REQUEST_RESERVED) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return ESTALE;
	}

	/* The common core discards this completion after success; the backend must never call it again. */
	if (session->pending == 0)
		__builtin_trap();
	kern_memset(request, 0, sizeof(*request));
	session->pending--;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Cancel may release the final close hold and actual reservation capacity independently of a callback. */
	waitq_wake_all(&controller->worker.retired);
	drv_gpu_capacity_changed(controller->gpu);

	/* Succeeded: definite nonacceptance withdrew the original callback without signaling completion. */
	return 0;
}

/* Reports bounded marker capacity only for a live, owned Vulkan completion domain. */
static int
job_capacity(
	void *opaque,
	void *private_session,
	uint32_t timeline,
	unsigned *available)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	unsigned long enabled;
	uint64_t mask;

	/* A refused query owns no slot and reports no usable marker capacity. */
	if (available == NULL || timeline == 0 || timeline >= 64)
		return EINVAL;
	*available = 0;
	controller = opaque;
	session = private_session;
	mask = UINT64_C(1) << timeline;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	/* Queue creation owns this domain for the retained backend session. */
	if ((session->timelines & mask) == 0) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EINVAL;
	}

	/* Closed native admission cannot advertise capacity even if slots are numerically free. */
	if (session->stopping || controller->worker.uncertain ||
	    !controller->space.native->hardware.ready || controller->space.native->hardware.faulted) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EIO;
	}

	/* The snapshot promises no ownership; reserve competes for the same guarded pool. */
	*available = reservation_capacity(&controller->worker);

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: the caller may attempt a nonblocking reservation without consuming a slot. */
	return 0;
}

/* Resolves exact slot, owner and original callback identity without dereferencing an untrusted token. */
static struct bcm2711_render_request *
find_reservation(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct bcm2711_render_request *request;
	uint32_t slot;

	/* Null or already withdrawn callbacks cannot name a retained reservation. */
	if (token == NULL || completion == NULL)
		return NULL;

	/* The IRQ guard excludes slot reuse throughout identity validation and the caller's transition. */
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		request = &controller->worker.requests[slot];
		if (token != request || request->session != session ||
		    request->completion != completion || !request->supervised)
			continue;
		return request;
	}

	/* Succeeded: no retained slot matches this exact opaque reservation identity. */
	return NULL;
}

/* Publishes an owned reservation into the ordered worker FIFO without allocation. */
static void
queue_marker(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_request *request,
	int error)
{
	/* A reserved or uncertain unpublished callback now becomes posted work for stop/drain. */
	request->execute = execute_marker;
	request->dispose = dispose_marker;
	request->payload = NULL;
	request->canceled = error;
	request->state = REQUEST_QUEUED;
	request->next = NULL;
	if (controller->worker.tail == NULL) {
		controller->worker.head = request;
	} else {
		controller->worker.tail->next = request;
	}

	/* The marker retains the original pending hold until callback delivery ends. */
	controller->worker.tail = request;
}

/* Completes an ordered marker only after the single worker retired all preceding native commands. */
static int
execute_marker(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	void *payload,
	bool *retired)
{
	UNUSED_PARAMETER(controller);
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(payload);

	/* This marker launches no DMA; prior command execution supplied the actual native retirement proof. */
	*retired = true;

	/* Succeeded: an ordered marker needs no additional native operation to retire. */
	return 0;
}

/* Releases a marker with no prepared payload or independent native allocation. */
static int
dispose_marker(
	struct bcm2711_render_device *controller,
	void *payload,
	bool retired)
{
	UNUSED_PARAMETER(controller);
	UNUSED_PARAMETER(payload);
	UNUSED_PARAMETER(retired);

	/* Succeeded: the marker never acquired any DMA storage that needs disposal. */
	return 0;
}

/* Counts usable supervised slots while preserving eight slots for ordinary commands. */
static unsigned
reservation_capacity(
	struct bcm2711_render_worker *worker)
{
	uint32_t slot;
	unsigned free_slots;
	unsigned supervised;
	unsigned capacity;

	/* Live queued and finishing supervised markers consume the same bound as unpublished reservations. */
	free_slots = 0;
	supervised = 0;
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		if (worker->requests[slot].state == REQUEST_FREE)
			free_slots++;
		if (worker->requests[slot].supervised)
			supervised++;
	}

	/* A full producer reservation pool cannot starve decoder command admission. */
	if (supervised >= BCM2711_RENDER_RESERVATIONS)
		return 0;
	capacity = BCM2711_RENDER_RESERVATIONS - supervised;
	if (capacity > free_slots)
		capacity = free_slots;

	/* Succeeded: this numerical snapshot grants no callback or slot ownership. */
	return capacity;
}

/* Serves the permanent FIFO and sleeps only with the queue condition guard held. */
static void
worker_main(
	void *opaque)
{
	struct bcm2711_render_device *controller;
	unsigned long enabled;
	uint64_t observed;
	int error;

	/* The registered controller and its thread retain the queue for the entire kernel lifetime. */
	controller = opaque;

	/* Ordered work always uses the same one-step retirement path before waiting for producers. */
	while (true) {
		error = bcm2711_render_worker_step(controller);
		if (error == 0)
			continue;
		if (error != EAGAIN)
			__builtin_trap();
		enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

		/* A producer racing the empty dequeue cannot disappear between observation and sleep. */
		while (controller->worker.head == NULL) {
			observed = waitq_sequence(&controller->worker.available);
			error = waitq_sleep(&controller->worker.available, &controller->space.native->hardware.guard, observed, 0, 0);
			if (error != 0 && error != EAGAIN)
				__builtin_trap();
		}

		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
	}
}

/* Gives the single worker one immutable active request while retaining its session callback count. */
static struct bcm2711_render_request *
take_request(
	struct bcm2711_render_device *controller)
{
	struct bcm2711_render_request *request;
	unsigned long enabled;

	/* The permanent FIFO owner serializes dequeue against publication and queued cancellation. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	request = controller->worker.head;
	if (request != NULL) {
		controller->worker.head = request->next;
		if (controller->worker.head == NULL)
			controller->worker.tail = NULL;
		request->next = NULL;
		request->state = REQUEST_ACTIVE;
	}

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: a nonnull request retains its complete payload and common callback. */
	return request;
}

/* Ends the common callback before releasing its slot and the session's last pending hold. */
static void
finish_request(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_request *request,
	int error)
{
	struct drv_gpu_completion *completion;
	struct bcm2711_render_session *session;
	unsigned long enabled;

	/* FINISHING keeps the slot and session alive across reentrant common completion observers. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	if (request->state != REQUEST_ACTIVE)
		__builtin_trap();
	request->state = REQUEST_FINISHING;
	completion = request->completion;
	session = request->session;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* The common completion owns its own lifetime and may wake a concurrently closing thread. */
	drv_gpu_complete(completion, error);
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	if (request->state != REQUEST_FINISHING || session->pending == 0)
		__builtin_trap();
	kern_memset(request, 0, sizeof(*request));
	session->pending--;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Capacity and final close observers see the fully retired slot and callback count. */
	waitq_wake_all(&controller->worker.retired);
	drv_gpu_capacity_changed(controller->gpu);
}
