/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises actual common admission and supervision with explicit scheduler boundaries. */
#define GPU_JOB_HELPERS_ONLY
#include "gpu-job.c"
#include "../../../src/drivers/gpu/gpu.c"

/* The scheduler peer changes this clock only at an explicit scenario boundary. */
extern uint64_t gpu_test_ticks;

/* One second of the scheduler peer's clock, in the ticks the core counts deadlines in. */
#define TEST_SECOND_TICKS	kern_ms_to_ticks(1000U)

/* A selected wait boundary models a different caller retiring actual production ownership. */
extern int (*gpu_test_wait_hook)(struct wait_queue *, struct spinlock *, uint64_t, uint64_t, unsigned);

/* Nonzero actions deliver one callback or release one actual common observer during sleep. */
static unsigned capacity_action;

/* The wait scenario retains the same observer object returned by production lookup. */
static struct drv_gpu_completion *capacity_observer;

/* These counters distinguish context stopping from whole-device fallback and checked reset. */
static unsigned local_starts;
static unsigned local_polls;
static unsigned global_faults;
static unsigned checked_resets;

/* An IRQ-like report inside reset must survive the callback returning success. */
static unsigned reset_inject_fault;

/* Raw command ownership remains live even when no framework job was submitted. */
static void *raw_session;
static unsigned raw_released;
static unsigned raw_quarantined;

/* A selected worker allocation error exercises common close-time fallback. */
extern int gpu_test_thread_error;

/* Counts opaque raw streams accepted by the bounded peer; they create native ownership without a callback. */
static unsigned raw_commands;

/* Counts per-context isolations; each ends the isolated context's callbacks without a device fault. */
static unsigned isolations;

/* Nonzero models a backend (i915) whose stop poll counts an unpublished reservation as pending work. */
static unsigned poll_counts_reserved;

static int capacity_snapshot(void *opaque, void *session, uint32_t domain, unsigned *available);
static int supervised_reserve(void *opaque, void *session, uint32_t domain, struct drv_gpu_completion *completion, void **reservation);
static int local_begin(void *opaque, void *session, int error);
static int local_poll(void *opaque, void *session);
static void global_fault(void *opaque, int error);
static int checked_reset(void *opaque);
static int local_isolate(void *opaque, void *session);
static void supervised_close(void *opaque, void *session);
static void supervised_drain(void *opaque, void *session);
static int raw_command(void *opaque, void *session, const void *buffer, uint32_t bytes);
static void raw_submit(struct test_file *file);
static int raw_interleave(struct wait_queue *queue, struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags);
static int capacity_call(struct test_file *file, unsigned flags, uint64_t observed, uint64_t timeout, struct gpu_job_capacity *result);
static int capacity_interleave(struct wait_queue *queue, struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags);
static int capacity_interrupted(struct wait_queue *queue, struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags);
static uint64_t submit_job(struct test_file *file, unsigned commit);
static void deliver_session(struct test_file *file, int error);

/*
 * Checks independent capacity, logical timeout, callback retention and checked recovery.
 */
int
main(
	void)
{
	struct drv_gpu_ops operations;
	struct drv_gpu_job_ops job_operations;
	struct drv_gpu_command_ops commands;
	struct drv_gpu_recovery_ops recovery;
	struct test_backend backend;
	struct test_file first;
	struct test_file second;
	struct test_file refused;
	struct process process;
	struct gpu_job_reserve request;
	struct gpu_job_capacity capacity;
	struct gpu_job_policy policy;
	struct gpu_command_wait observation;
	struct gpu_session *owner;
	struct gpu_session *peer;
	uint64_t sequences[GPU_SUBMIT_MAX];
	uint64_t before;
	uint64_t first_sequence;
	uint64_t second_sequence;
	uint64_t next;
	unsigned index;
	short readiness;
	int status;
	int error;

	/* Actual descriptor and cdev code establish two independent process-like opens. */
	memset(&process, 0, sizeof(process));
	process.fd = filedesc_create(&process);
	assert(process.fd != NULL);
	gpu_test_set_process(&process);
	memset(&backend, 0, sizeof(backend));

	/* The bounded backend reserves four real common callback obligations. */
	memset(&job_operations, 0, sizeof(job_operations));
	job_operations.reserve = supervised_reserve;
	job_operations.commit = job_commit;
	job_operations.cancel = job_cancel;
	job_operations.capacity = capacity_snapshot;
	commands.submit = fence_submit;
	commands.drain = supervised_drain;

	/* Local confirmation and global quarantine remain separate operations. */
	memset(&recovery, 0, sizeof(recovery));
	recovery.stop_begin = local_begin;
	recovery.stop_poll = local_poll;
	recovery.fault = global_fault;
	recovery.reset = checked_reset;

	/* Registration uses the production version and validates every advertised callback. */
	memset(&operations, 0, sizeof(operations));
	operations.version = DRV_GPU_INTERFACE_VERSION;
	operations.size = sizeof(operations);
	operations.capabilities = GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY | GPU_CAP_NOTIFICATION | GPU_CAP_FENCE | GPU_CAP_COMMAND;
	operations.open = backend_open;
	operations.close = supervised_close;
	operations.get_info = backend_get_info;
	operations.command = raw_command;
	operations.commands = &commands;
	operations.jobs = &job_operations;
	operations.recovery = &recovery;

	/* Raw-only recovery must still provide quarantine before a failure permits destruction. */
	operations.capabilities = 0U;
	operations.jobs = NULL;
	operations.commands = NULL;
	recovery.fault = NULL;
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == EINVAL && job_device == NULL);

	/* Restore the complete supervised backend after testing the non-JOB contract. */
	operations.capabilities = GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY | GPU_CAP_NOTIFICATION | GPU_CAP_FENCE | GPU_CAP_COMMAND;
	operations.jobs = &job_operations;
	operations.commands = &commands;
	recovery.fault = global_fault;
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == 0);
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0);
	error = open_file(&second, "gpu0", O_RDWR);
	assert(error == 0);
	owner = first.file.f_data;
	peer = second.file.f_data;

	/* Policy is queried through its actual fixed-width public ioctl. */
	memset(&policy, 0, sizeof(policy));
	policy.version = GPU_ABI_VERSION;
	policy.size = sizeof(policy);
	error = fence_ioctl(&first, GPU_JOB_POLICY, &policy);
	assert(error == 0);
	assert(policy.reservation_timeout_ns == 10000000000ULL);
	assert(policy.execution_timeout_ns == 60000000000ULL);
	assert(policy.stop_timeout_ns == 10000000000ULL);

	/* Another open can own every backend slot while the waiter has no readable completion. */
	for (index = 0U; index < 4U; index++)
		sequences[index] = submit_job(&first, 0U);

	error = capacity_call(&second, GPU_JOB_CAPACITY_QUERY, 0U, 0U, &capacity);
	assert(error == 0 && capacity.available == 0U);
	before = capacity.sequence;
	job_request(&request, -1, 0U);
	error = fence_ioctl(&second, GPU_JOB_RESERVE, &request);
	assert(error == EAGAIN);
	error = capacity_call(&second, 0U, before, 0U, &capacity);
	assert(error == EAGAIN);
	readiness = 0;
	error = gpu_poll(&second.file, POLLIN, &readiness);
	assert(error == 0 && readiness == 0);

	/* Failed observation copyout neither reserves storage nor consumes the capacity generation. */
	reject_copyout = 1U;
	error = capacity_call(&second, GPU_JOB_CAPACITY_QUERY, 0U, 0U, &capacity);
	assert(error == EFAULT && job_device->capacity_sequence == before);

	/* A finite capacity deadline reports timeout while every native job remains unaccepted. */
	capacity_action = 3U;
	gpu_test_wait_hook = capacity_interrupted;
	error = capacity_call(&second, 0U, before, 100000000U, &capacity);
	gpu_test_wait_hook = NULL;
	assert(error == ETIMEDOUT && job_device->capacity_sequence == before);

	/* Signal interruption preserves the same retry token without manufacturing completion. */
	capacity_action = 4U;
	gpu_test_wait_hook = capacity_interrupted;
	error = capacity_call(&second, 0U, before, UINT64_MAX, &capacity);
	gpu_test_wait_hook = NULL;
	assert(error == EINTR && job_device->capacity_sequence == before);

	/* A canceled foreign reservation wakes the capacity channel without local POLLIN. */
	error = job_action(&first, GPU_JOB_CANCEL, sequences[0], 0U);
	assert(error == 0);
	error = capacity_call(&second, 0U, before, 0U, &capacity);
	assert(error == 0 && capacity.available == 1U && capacity.sequence != before);

	/* Backend domain validation is independent from the old Venus limit of sixty-three. */
	job_request(&request, -1, 0U);
	request.timeline = 77U;
	error = fence_ioctl(&second, GPU_JOB_RESERVE, &request);
	assert(error == 0);
	error = job_action(&second, GPU_JOB_CANCEL, request.sequence, 0U);
	assert(error == 0);

	/* Definite nonacceptance retires all remaining foreign reservations. */
	for (index = 1U; index < 4U; index++) {
		error = job_action(&first, GPU_JOB_CANCEL, sequences[index], 0U);
		assert(error == 0);
	}

	/* Sixty-three terminal records plus one pending record exhaust common storage. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		sequences[index] = submit_job(&second, 1U);
		if (index + 1U < GPU_SUBMIT_MAX)
			deliver_session(&second, 0);
	}

	error = capacity_call(&second, GPU_JOB_CAPACITY_QUERY, 0U, 0U, &capacity);
	assert(error == 0 && capacity.available == 0U);
	before = capacity.sequence;
	capacity_action = 1U;
	gpu_test_wait_hook = capacity_interleave;
	error = capacity_call(&second, 0U, before, UINT64_MAX, &capacity);
	gpu_test_wait_hook = NULL;
	assert(error == 0 && capacity.available == 0U && capacity.sequence != before);

	/* Terminal transition wakes the same U caller to reap instead of waiting forever for its own CONSUME. */
	memset(&observation, 0, sizeof(observation));
	observation.version = GPU_ABI_VERSION;
	observation.size = sizeof(observation);
	observation.sequence = sequences[0];
	error = gpu_completion_observe(peer, &observation, &capacity_observer);
	assert(error == 0);
	error = job_observe(&second, sequences[0], 1U, &status);
	assert(error == 0 && status == 0);

	/* A pinned consumed result has no free record and must not cause a persistent level-triggered spin. */
	error = capacity_call(&second, GPU_JOB_CAPACITY_QUERY, 0U, 0U, &capacity);
	assert(error == 0 && capacity.available == 0U);
	before = capacity.sequence;
	error = capacity_call(&second, 0U, before, 0U, &capacity);
	assert(error == EAGAIN);
	capacity_action = 2U;
	gpu_test_wait_hook = capacity_interleave;
	error = capacity_call(&second, 0U, before, UINT64_MAX, &capacity);
	gpu_test_wait_hook = NULL;
	assert(error == 0 && capacity.available == 1U);

	/* The remaining public results retire through the same production consume operation. */
	for (index = 1U; index < GPU_SUBMIT_MAX; index++) {
		error = job_observe(&second, sequences[index], 1U, &status);
		assert(error == 0 && status == 0);
	}

	/* A normally completing job may run beyond the former ten-second execution limit. */
	first_sequence = submit_job(&first, 1U);
	gpu_test_ticks += 11U * TEST_SECOND_TICKS;
	next = gpu_monitor_step(job_device);
	assert(next > gpu_test_ticks && owner->error == 0 && global_faults == 0U);
	deliver_session(&first, 0);
	error = job_observe(&first, first_sequence, 1U, &status);
	assert(error == 0 && status == 0);

	/* A committed owner's expiry does not fail a later independent submission. */
	first_sequence = submit_job(&first, 1U);
	gpu_test_ticks += 5U * TEST_SECOND_TICKS;
	second_sequence = submit_job(&second, 1U);
	gpu_test_ticks += 55U * TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(owner->error == ETIMEDOUT && peer->error == 0);
	assert(local_starts == 1U && global_faults == 0U);
	error = job_observe(&first, first_sequence, 1U, &status);
	assert(error == 0 && status == ETIMEDOUT);
	assert(owner->completions[0].sequence == first_sequence);
	assert(owner->completions[0].backend_owned != 0U);

	/* A successful late hardware completion retires its pin without replacing the logical timeout. */
	deliver_session(&first, 0);
	assert(owner->completions[0].sequence == 0U);
	deliver_session(&second, 0);
	error = job_observe(&second, second_sequence, 1U, &status);
	assert(error == 0 && status == 0);
	gpu_test_ticks += TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(owner->stop_state == GPU_STOP_FINISHED && local_polls == 1U);
	assert(job_device->error == 0 && global_faults == 0U);

	/* Independent work continues on the same device after confirmed local retirement. */
	second_sequence = submit_job(&second, 1U);
	deliver_session(&second, 0);
	error = job_observe(&second, second_sequence, 1U, &status);
	assert(error == 0 && status == 0);

	/* Unknown native acceptance in an expired reservation cannot claim a graceful stop. */
	second_sequence = submit_job(&second, 0U);
	gpu_test_ticks += 10U * TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(peer->error == ETIMEDOUT && global_faults == 0U);
	gpu_test_ticks += 10U * TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(global_faults == 1U && job_device->error != 0);
	error = job_observe(&second, second_sequence, 1U, &status);
	assert(error == 0 && status == ETIMEDOUT);

	/* Reset cannot pass the shared ownership gate while either failed open remains. */
	error = open_file(&refused, "gpu0", O_RDWR);
	assert(error != 0 && checked_resets == 0U);
	close_file(&first);
	close_file(&second);

	/* A new fault with the same errno during reset cannot be erased by callback success. */
	reset_inject_fault = 1U;
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error != 0 && checked_resets == 1U && job_device->error != 0);
	reset_inject_fault = 0U;
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0 && checked_resets == 2U && job_device->error == 0);
	first_sequence = submit_job(&first, 1U);
	deliver_session(&first, 0);
	error = job_observe(&first, first_sequence, 1U, &status);
	assert(error == 0 && status == 0);

	/* An unconfirmed stop isolates only the failing context once the backend can quarantine per context. */
	recovery.isolate = local_isolate;
	error = open_file(&second, "gpu0", O_RDWR);
	assert(error == 0);
	peer = second.file.f_data;
	second_sequence = submit_job(&second, 0U);
	gpu_test_ticks += 10U * TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(peer->error == ETIMEDOUT && isolations == 0U);
	gpu_test_ticks += 10U * TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(isolations == 1U && global_faults == 1U && job_device->error == 0);
	assert(peer->stop_state == GPU_STOP_QUARANTINED && job_device->quarantined == 1U);
	error = job_observe(&second, second_sequence, 1U, &status);
	assert(error == 0 && status == ETIMEDOUT);

	/* The healthy peer keeps submitting while the isolated context is retained. */
	first_sequence = submit_job(&first, 1U);
	deliver_session(&first, 0);
	error = job_observe(&first, first_sequence, 1U, &status);
	assert(error == 0 && status == 0);

	/* A fresh open with no other owner reclaims the quarantined capacity through checked reset. */
	close_file(&second);
	close_file(&first);
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0 && checked_resets == 3U);
	assert(job_device->quarantined == 0U && job_device->error == 0);

	/* An expired reservation on a proven-idle context is withdrawn by the common layer through cancel. */
	owner = first.file.f_data;
	second_sequence = submit_job(&first, 0U);
	gpu_test_ticks += 10U * TEST_SECOND_TICKS;
	(void)gpu_monitor_step(job_device);
	assert(owner->error == ETIMEDOUT && job_device->error == 0);
	(void)gpu_monitor_step(job_device);
	assert(owner->stop_state == GPU_STOP_FINISHED && global_faults == 1U);
	for (index = 0U; index < 4U; index++)
		assert(jobs[index].completion == NULL);
	error = job_observe(&first, second_sequence, 1U, &status);
	assert(error == 0 && status == ETIMEDOUT);
	close_file(&first);

	/*
	 * BUG-077: a producer that reached the GPU closes between reserve and
	 * commit.  Close withdraws the reservation itself, so a backend whose stop
	 * poll counts it as pending still confirms the stop without isolation.
	 */
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0);
	first_sequence = submit_job(&first, 1U);
	deliver_session(&first, 0);
	error = job_observe(&first, first_sequence, 1U, &status);
	assert(error == 0 && status == 0);
	(void)submit_job(&first, 0U);
	before = isolations;
	poll_counts_reserved = 1U;
	close_file(&first);
	poll_counts_reserved = 0U;
	assert(isolations == before && global_faults == 1U && job_device->error == 0);
	assert(job_device->quarantined == 0U);
	for (index = 0U; index < 4U; index++)
		assert(jobs[index].completion == NULL);
	error = drv_gpu_unregister(job_device);
	assert(error == 0);

	/* A context that never reached the GPU closes without any backend stop handshake. */
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == 0);
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0);
	raw_session = NULL;
	before = local_starts;
	close_file(&first);
	assert(local_starts == before && job_device->error == 0);

	/* The withdrawn previous device armed one quarantine; this close added none. */
	assert(global_faults == 2U);
	error = drv_gpu_unregister(job_device);
	assert(error == 0);

	/* A raw-only open must start supervision and await actual native quiescence before close. */
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == 0);
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0 && job_device->monitor == NULL);
	owner = first.file.f_data;
	raw_session = owner->backend;
	raw_released = 0U;
	raw_quarantined = 0U;
	raw_submit(&first);
	gpu_test_wait_hook = raw_interleave;
	close_file(&first);
	gpu_test_wait_hook = NULL;
	assert(raw_released == 1U && raw_quarantined == 0U);
	raw_session = NULL;
	error = drv_gpu_unregister(job_device);
	assert(error == 0);

	/* Worker allocation failure still quarantines raw native ownership before destruction. */
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == 0);
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0 && job_device->monitor == NULL);
	owner = first.file.f_data;
	raw_session = owner->backend;
	raw_released = 0U;
	raw_quarantined = 0U;
	raw_submit(&first);
	gpu_test_thread_error = ENOMEM;
	close_file(&first);
	gpu_test_thread_error = 0;
	assert(raw_released == 0U && raw_quarantined == 1U);
	raw_session = NULL;
	error = drv_gpu_unregister(job_device);
	assert(error == 0);
	/* Withdrawal cannot publish global teardown permission before backend quarantine is armed. */
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == 0);
	error = open_file(&first, "gpu0", O_RDWR);
	assert(error == 0);
	owner = first.file.f_data;
	raw_session = owner->backend;
	raw_released = 0U;
	raw_quarantined = 0U;
	error = drv_gpu_unregister(job_device);
	assert(error == EBUSY && raw_quarantined == 1U);
	close_file(&first);
	raw_session = NULL;
	error = drv_gpu_unregister(job_device);
	assert(error == 0);

	/* Final descriptor teardown leaves no peer allocation or lock owner. */
	filedesc_destroy(process.fd);
	gpu_test_set_process(NULL);
	assert(allocations == 0U && held_spinlocks == 0U);
	puts("GPU supervision: foreign capacity, 64-record self-reap, observer pin, backend domain, 10s/60s policy, local drain, late callback, global fallback, reset gate, per-context isolation, idle reset reclaim, reservation reclaim, reserved close and metadata-only close PASS");

	/* Succeeded: all production ownership retired at explicitly observed boundaries. */
	return 0;
}

/* Samples actual peer storage while allowing a non-Venus completion domain. */
static int
capacity_snapshot(
	void *opaque,
	void *session,
	uint32_t domain,
	unsigned *available)
{
	unsigned index;

	(void)opaque;
	(void)session;

	/* The framework must delegate domain validity to the selected backend. */
	assert(held_spinlocks == 0U);
	if (domain != 1U && domain != 77U)
		return EINVAL;

	/* A free backend slot is independent from common per-open record capacity. */
	*available = 0U;
	for (index = 0U; index < 4U; index++) {
		if (jobs[index].completion == NULL)
			*available = 1U;
	}

	/* Succeeded: no capacity was consumed by this snapshot. */
	return 0;
}

/* Delegates exact callback ownership after validating this backend's own domain namespace. */
static int
supervised_reserve(
	void *opaque,
	void *session,
	uint32_t domain,
	struct drv_gpu_completion *completion,
	void **reservation)
{
	int error;

	/* Domain seventy-seven demonstrates absence of the old framework-wide Venus limit. */
	if (domain != 1U && domain != 77U)
		return EINVAL;

	/* The original finite peer verifies the unchanged reserve/commit/cancel contract. */
	error = job_reserve(opaque, session, 1U, completion, reservation);
	if (error != 0)
		return error;

	/* Succeeded: one exact completion is retained by the bounded backend. */
	return 0;
}

/* Models stopping new context work after all already-admitted core callbacks have left. */
static int
local_begin(
	void *opaque,
	void *session,
	int error)
{
	(void)opaque;
	(void)session;

	/* The actual common monitor must call this without its registry lock. */
	assert(held_spinlocks == 0U && error != 0);
	local_starts++;

	/* Succeeded: local confirmation may now inspect actual retained peer jobs. */
	return 0;
}

/* Confirms retirement only when the context no longer owns any backend callback. */
static int
local_poll(
	void *opaque,
	void *session)
{
	unsigned index;

	(void)opaque;

	/* Native-access confirmation remains a backend responsibility. */
	assert(held_spinlocks == 0U);
	local_polls++;

	/* Descriptor absence cannot establish that opaque native work has stopped. */
	if (raw_session == session && raw_released == 0U)
		return EAGAIN;

	/* Published peer jobs remain retained until actual completion; unpublished reservations block idle only when modeled. */
	for (index = 0U; index < 4U; index++) {
		if (jobs[index].session == session &&
		    jobs[index].completion != NULL &&
		    (jobs[index].committed != 0U || poll_counts_reserved != 0U))
			return EAGAIN;
	}

	/* Succeeded: this bounded peer retains no native or callback owner. */
	return 0;
}

/* Models i915's drain, which waits for every pending request: an uncommitted reservation would never retire. */
static void
supervised_drain(
	void *opaque,
	void *session)
{
	unsigned index;

	/* Close must have withdrawn the reservation before draining (BUG-077). */
	if (poll_counts_reserved != 0U) {
		for (index = 0U; index < 4U; index++) {
			assert(jobs[index].session != session ||
			    jobs[index].completion == NULL ||
			    jobs[index].committed != 0U);
		}
	}

	/* The shared peer delivers the remaining callbacks. */
	job_drain(opaque, session);
}

/* Models transport-wide quarantine and ends every retained peer callback. */
static void
global_fault(
	void *opaque,
	int error)
{
	/* The real framework's terminal result remains authoritative through backend retirement. */
	global_faults++;
	if (raw_session != NULL)
		raw_quarantined = 1U;

	/* Quarantine precedes releasing any context or uncertain native backing. */
	job_fault(opaque, error);

	/* Succeeded: no retained peer callback can arrive after this barrier. */
	return;
}

/* Quarantines one context of the bounded peer, ending its callbacks with the context's error. */
static int
local_isolate(
	void *opaque,
	void *session)
{
	unsigned index;

	(void)opaque;

	/* Isolation is a driver callback outside every common spinlock. */
	assert(held_spinlocks == 0U);
	isolations++;

	/* Each retained callback of this context alone publishes its terminal error. */
	for (index = 0U; index < 4U; index++) {
		if (jobs[index].session == session && jobs[index].completion != NULL)
			job_deliver(&jobs[index], EIO);
	}

	/* Succeeded: peers keep their callbacks while this context is quarantined. */
	return 0;
}

/* Confirms that common retirement excluded every old open before hardware reinitialization. */
static int
checked_reset(
	void *opaque)
{
	struct test_backend *backend;
	unsigned index;

	/* The backend's own lifetime counters provide an independent reset precondition. */
	backend = opaque;
	assert(held_spinlocks == 0U && backend->opens == backend->closes);
	for (index = 0U; index < 4U; index++)
		assert(jobs[index].completion == NULL);

	/* Only this successful callback may clear the common device error. */
	checked_resets++;

	/* A same-value report models an interrupt after hardware restart but before core publication. */
	if (reset_inject_fault != 0U)
		drv_gpu_report_error(job_device, job_device->error);

	/* Succeeded: the modeled hardware can create a new independent context. */
	return 0;
}

/* Sends the public capacity request without replacing its condition checks. */
static int
capacity_call(
	struct test_file *file,
	unsigned flags,
	uint64_t observed,
	uint64_t timeout,
	struct gpu_job_capacity *result)
{
	int error;

	/* The caller owns only an observation, never a reserved submission credit. */
	memset(result, 0, sizeof(*result));
	result->version = GPU_ABI_VERSION;
	result->size = sizeof(*result);
	result->flags = flags;
	result->domain = 1U;
	result->observed_sequence = observed;
	result->timeout_ns = timeout;
	error = fence_ioctl(file, GPU_JOB_CAPACITY, result);
	if (error != 0)
		return error;

	/* Succeeded: the actual kernel condition produced this snapshot. */
	return 0;
}

/* Schedules exactly one real callback or observer release across the atomic sleep boundary. */
static int
capacity_interleave(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	unsigned long irq;
	unsigned index;

	/* The production waiter holds no backend mutex and requests interruptible indefinite observation. */
	assert(queue == &job_device->capacity_waitq);
	assert(flags == WAITQ_INTERRUPTIBLE && deadline == 0U);
	assert(queue->sequence == observed && capacity_action != 0U);
	spin_unlock_irqrestore(lock, 1U);

	/* Delivery publishes a terminal record; it deliberately leaves CONSUME to the original caller. */
	if (capacity_action == 1U) {
		for (index = 0U; index < 4U; index++) {
			if (jobs[index].completion != NULL) {
				job_deliver(&jobs[index], 0);
				drv_gpu_capacity_changed(job_device);
			}
		}
	} else {
		/* Releasing the final observer makes an already consumed record reusable. */
		assert(capacity_action == 2U);
		gpu_completion_observer_leave(capacity_observer, 0U, 0U);
		capacity_observer = NULL;
	}

	/* Atomic sleep returns with its original condition lock held. */
	capacity_action = 0U;
	irq = spin_lock_irqsave(lock);
	(void)irq;

	/* The changed sequence causes production code to resample both capacity owners. */
	return EAGAIN;
}

/* Ends an atomic capacity wait without altering either reservation owner. */
static int
capacity_interrupted(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	(void)lock;

	/* The actual common waiter selected an unchanged interruptible capacity condition. */
	assert(queue == &job_device->capacity_waitq);
	assert(queue->sequence == observed && flags == WAITQ_INTERRUPTIBLE);

	/* A scheduler timeout advances the clock without delivering any GPU completion. */
	if (capacity_action == 3U) {
		assert(deadline > gpu_test_ticks);
		gpu_test_ticks = deadline;
		capacity_action = 0U;
		return ETIMEDOUT;
	}

	/* An interrupted indefinite wait retains the condition lock and its native nonacceptance. */
	assert(capacity_action == 4U && deadline == 0U);
	capacity_action = 0U;
	return EINTR;
}

/* Creates one ordinary optional-fence job through the actual cdev operations. */
static uint64_t
submit_job(
	struct test_file *file,
	unsigned commit)
{
	struct gpu_job_reserve request;
	int error;

	/* A successful reserve creates the exact sequence consumed by every later operation. */
	job_request(&request, -1, 0U);
	error = fence_ioctl(file, GPU_JOB_RESERVE, &request);
	assert(error == 0);

	/* The uncommitted case models a producer stopping before publication. */
	if (commit != 0U) {
		error = job_action(file, GPU_JOB_COMMIT, request.sequence, 0U);
		assert(error == 0);
	}

	/* Succeeded: the returned identity is retained by the real common completion ledger. */
	return request.sequence;
}

/* Delivers all jobs owned by one open without changing independent contexts. */
static void
deliver_session(
	struct test_file *file,
	int error)
{
	struct gpu_session *session;
	unsigned index;

	/* The real session carries the immutable backend identity used by each reservation. */
	session = file->file.f_data;
	for (index = 0U; index < 4U; index++) {
		if (jobs[index].completion != NULL && jobs[index].session == session->backend) {
			job_deliver(&jobs[index], error);
			drv_gpu_capacity_changed(job_device);
		}
	}

	/* Succeeded: each backend release notified the common capacity channel afterward. */
	return;
}

/* Refuses context destruction before native stop or whole-device quarantine is proven. */
static void
supervised_close(
	void *opaque,
	void *session)
{
	/* An empty callback ledger alone is insufficient for this raw native owner. */
	if (session == raw_session)
		assert(raw_released != 0U || raw_quarantined != 0U);

	/* The original backend independently checks final resource and session ownership. */
	backend_close(opaque, session);

	/* Succeeded: native ownership ended before the context wrapper was destroyed. */
	return;
}

/* Accepts one opaque stream; the bounded peer keeps no callback for it. */
static int
raw_command(
	void *opaque,
	void *session,
	const void *buffer,
	uint32_t bytes)
{
	(void)opaque;
	(void)session;
	(void)buffer;

	/* The actual common core copies the stream before this synchronous receipt. */
	assert(held_spinlocks == 0U && bytes == 4U);
	raw_commands++;

	/* Succeeded: raw native ownership now exists without any completion record. */
	return 0;
}

/* Sends one minimal opaque stream through the public command request. */
static void
raw_submit(
	struct test_file *file)
{
	struct gpu_command request;
	uint32_t stream;
	int error;

	/* The stream bytes only need to exist; the peer never interprets them. */
	stream = 0U;
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.address = (uint64_t)(uintptr_t)&stream;
	request.bytes = sizeof(stream);
	error = fence_ioctl(file, GPU_COMMAND, &request);
	assert(error == 0);

	/* Succeeded: the core recorded native ownership for the final stop proof. */
	return;
}

/* Completes opaque native work only after common close reaches its actual stop wait. */
static int
raw_interleave(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	unsigned long irq;

	/* The close barrier must retain the context while the driver awaits true quiescence. */
	assert(queue == &job_device->monitor_waitq);
	assert(queue->sequence == observed && flags == 0U);
	assert(deadline > gpu_test_ticks && raw_released == 0U);
	spin_unlock_irqrestore(lock, 1U);

	/* The peer models a real stop acknowledgement, distinct from descriptor retirement. */
	raw_released = 1U;
	gpu_test_ticks = deadline;

	/* The sleep collaborator restores the condition lock before production code rechecks. */
	irq = spin_lock_irqsave(lock);
	(void)irq;

	/* Succeeded: the next actual stop_poll can prove quiescence. */
	return EAGAIN;
}
