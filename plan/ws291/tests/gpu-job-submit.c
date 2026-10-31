/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU core's one-call jobs (GPU_JOB_SUBMIT) and batched retirement
 * (GPU_JOB_RETIRE) through the real gpu.c and cdev dispatch, with the ws014
 * job fixture's backend peer and a job command op whose outcome each case
 * chooses (plan/ws291/tests/run-gpu-job-host.sh builds it).
 */
#define GPU_JOB_HELPERS_ONLY
#include "../../ws014/tests/gpu-job.c"

/* The scheduler peer's clock, which a slow stream moves on. */
extern uint64_t gpu_test_ticks;

/* The core's reservation deadline as the host build of gpu.c has it (CONFIG_GPU_JOB_RESERVATION_MS's default). */
#define SUBMIT_RESERVATION_MS	10000U

/*
 * The outcome the job command op reports next, the error it returns with
 * it, the ticks the stream takes, how many streams it ran and the first
 * word of the last one.
 */
static uint32_t submit_outcome;
static int submit_error;
static uint64_t submit_ticks;
static unsigned submit_runs;
static uint32_t submit_first_word;

static int submit_command(void *opaque, void *session, const void *buffer, uint32_t bytes, uint32_t *outcome);
static int submit_call(struct test_file *file, int fd, uint64_t generation, const uint32_t *stream, uint32_t bytes,
		       struct gpu_job_submit *request);
static int retire_call(struct test_file *file, struct gpu_job_retire_entry *entries, uint32_t count, uint32_t *retired);

/*
 * Checks each outcome's job and payload, the errors before native work, the
 * capabilities the core reports, and batched retirement.
 */
int
main(
	void)
{
	struct drv_gpu_ops operations;
	struct drv_gpu_command_ops commands;
	struct drv_gpu_job_ops job_operations;
	struct drv_gpu_recovery_ops recovery;
	struct test_backend backend;
	struct test_file source;
	struct test_file receiver;
	struct process process;
	struct gpu_fence_create created;
	struct gpu_fence_state state;
	struct gpu_job_submit request;
	struct gpu_job_retire_entry entries[3];
	struct gpu_info info;
	uint32_t stream[4];
	uint32_t retired;
	uint64_t committed;
	int status;
	int error;

	/* The descriptor table and the cdev opens are real. */
	memset(&process, 0, sizeof(process));
	process.fd = filedesc_create(&process);
	assert(process.fd != NULL);
	gpu_test_set_process(&process);
	memset(&backend, 0, sizeof(backend));
	commands.submit = fence_submit;
	commands.drain = job_drain;
	memset(&job_operations, 0, sizeof(job_operations));
	job_operations.reserve = job_reserve;
	job_operations.commit = job_commit;
	job_operations.cancel = job_cancel;
	job_operations.command = submit_command;
	memset(&recovery, 0, sizeof(recovery));
	recovery.fault = job_fault;
	recovery.stop_begin = job_stop_begin;
	recovery.stop_poll = job_stop_poll;
	memset(&operations, 0, sizeof(operations));
	operations.version = DRV_GPU_INTERFACE_VERSION;
	operations.size = sizeof(operations);
	operations.capabilities = GPU_CAP_JOB | GPU_CAP_NOTIFICATION | GPU_CAP_FENCE | GPU_CAP_RESOURCE | GPU_CAP_COMMAND;
	operations.open = backend_open;
	operations.close = backend_close;
	operations.get_info = backend_get_info;
	operations.resource_create = backend_create;
	operations.resource_destroy = backend_destroy;
	operations.command = backend_command;
	operations.commands = &commands;
	operations.jobs = &job_operations;
	operations.recovery = &recovery;
	error = drv_gpu_register(&operations, &backend, &job_device);
	assert(error == 0);
	error = open_file(&source, "gpu0", O_RDWR);
	assert(error == 0);
	error = open_file(&receiver, "gpu0", O_RDWR);
	assert(error == 0);
	fence_create_request(&created);
	error = fence_ioctl(&source, GPU_FENCE_CREATE, &created);
	assert(error == 0);
	memset(stream, 0, sizeof(stream));
	stream[0] = 0x51U;

	/* The core adds the two bits only to jobs the backend reports; this backend reports none. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	error = fence_ioctl(&source, GPU_GET_INFO, &info);
	assert(error == 0 && (info.capabilities & (GPU_CAP_JOB_SUBMIT | GPU_CAP_JOB_RETIRE)) == 0U);

	/* Bad requests run nothing and keep nothing. */
	error = submit_call(&source, created.fd, 0U, stream, sizeof(stream), &request);
	assert(error == EINVAL && submit_runs == 0U && jobs[0].completion == NULL);
	error = submit_call(&source, created.fd, 1U, stream, 6U, &request);
	assert(error == EINVAL && submit_runs == 0U);
	job_rejection = EAGAIN;
	error = submit_call(&source, created.fd, 1U, stream, sizeof(stream), &request);
	assert(error == EAGAIN && submit_runs == 0U && jobs[0].completion == NULL);
	job_rejection = 0;

	/* Committed: the stream ran once, the job is the backend's, and its completion signals the payload. */
	submit_outcome = GPU_JOB_SUBMIT_COMMITTED;
	error = submit_call(&source, created.fd, 1U, stream, sizeof(stream), &request);
	assert(error == 0 && request.outcome == GPU_JOB_SUBMIT_COMMITTED && request.sequence != 0U);
	assert(submit_runs == 1U && submit_first_word == 0x51U && jobs[0].committed == 1U);
	committed = request.sequence;
	error = job_action(&source, GPU_JOB_COMMIT, committed, 0U);
	assert(error == EALREADY || error == EBUSY);

	/* Retirement: pending while it runs, retired once it ended, then missing. */
	memset(entries, 0, sizeof(entries));
	entries[0].sequence = committed;
	entries[1].sequence = committed + 1000U;
	error = retire_call(&source, entries, 2U, &retired);
	assert(error == 0 && retired == 0U);
	assert(entries[0].state == GPU_JOB_RETIRE_PENDING && entries[1].state == GPU_JOB_RETIRE_MISSING);
	job_deliver(&jobs[0], 0);
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, created.fd, 1U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_SIGNALED);
	memset(entries, 0, sizeof(entries));
	entries[0].sequence = committed;
	entries[1].sequence = committed;
	error = retire_call(&source, entries, 2U, &retired);
	assert(error == 0 && retired == 1U);
	assert(entries[0].state == GPU_JOB_RETIRE_RETIRED && entries[0].status == 0U);
	assert(entries[1].state == GPU_JOB_RETIRE_PENDING);
	error = job_observe(&source, committed, 1U, &status);
	assert(error == ENOENT);
	memset(entries, 0, sizeof(entries));
	entries[0].sequence = committed;
	error = retire_call(&source, entries, 0U, &retired);
	assert(error == EINVAL);
	error = fence_state_call(&receiver, GPU_FENCE_RESET, created.fd, 1U, 0, &state);
	assert(error == 0 && state.generation == 2U);

	/* Canceled and deferred: the stream's turn ended with nothing accepted; no record, the payload stays pending. */
	submit_outcome = GPU_JOB_SUBMIT_CANCELED;
	error = submit_call(&source, created.fd, 2U, stream, sizeof(stream), &request);
	assert(error == 0 && request.outcome == GPU_JOB_SUBMIT_CANCELED && jobs[0].completion == NULL);
	error = job_observe(&source, request.sequence, 0U, &status);
	assert(error == ENOENT);
	submit_outcome = GPU_JOB_SUBMIT_DEFERRED;
	error = submit_call(&source, created.fd, 2U, stream, sizeof(stream), &request);
	assert(error == 0 && request.outcome == GPU_JOB_SUBMIT_DEFERRED && jobs[0].completion == NULL);
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, created.fd, 2U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_PENDING);
	assert(submit_runs == 3U);

	/* A stream that failed before any of its work reached the GPU is canceled with its error; the open goes on. */
	submit_outcome = GPU_JOB_SUBMIT_CANCELED;
	submit_error = ENOMEM;
	error = submit_call(&source, created.fd, 2U, stream, sizeof(stream), &request);
	assert(error == 0 && request.outcome == GPU_JOB_SUBMIT_CANCELED && request.error == ENOMEM);
	assert(jobs[0].completion == NULL);

	/* A stream that failed after its work may have reached the GPU is a fault, whatever outcome it named. */
	submit_outcome = GPU_JOB_SUBMIT_DEFERRED;
	submit_error = EIO;
	error = submit_call(&source, created.fd, 2U, stream, sizeof(stream), &request);
	assert(error == 0 && request.outcome == GPU_JOB_SUBMIT_FAULTED && request.error == EIO);
	error = submit_call(&source, created.fd, 2U, stream, sizeof(stream), &request);
	assert(error != 0);

	/* After a checked recovery: a reservation whose deadline passed while its stream ran is a fault, not a commit. */
	close_file(&source);
	close_file(&receiver);
	drv_gpu_report_error(job_device, 0);
	error = open_file(&source, "gpu0", O_RDWR);
	assert(error == 0);
	error = open_file(&receiver, "gpu0", O_RDWR);
	assert(error == 0);
	submit_outcome = GPU_JOB_SUBMIT_COMMITTED;
	submit_error = 0;
	submit_ticks = kern_ms_to_ticks(SUBMIT_RESERVATION_MS) + 1U;
	error = submit_call(&source, -1, 0U, stream, sizeof(stream), &request);
	assert(error == 0 && request.outcome == GPU_JOB_SUBMIT_FAULTED && jobs[0].committed == 0U);
	submit_ticks = 0U;

	/* Everything retires: the opens, the device's registration, the descriptor table. */
	close_file(&source);
	close_file(&receiver);
	drv_gpu_retain(job_device);
	error = drv_gpu_unregister(job_device);
	assert(error == 0);
	drv_gpu_release(job_device);
	filedesc_destroy(process.fd);
	gpu_test_set_process(NULL);
	assert(allocations == 0U && held_spinlocks == 0U);

	puts("GPU job submit: rejected requests, committed, canceled (with its error), deferred, faulted and late-deadline outcomes, batched retirement PASS");
	return 0;
}

/* Runs a job's stream: counts it, and reports the chosen outcome or error. */
static int
submit_command(
	void *opaque,
	void *session,
	const void *buffer,
	uint32_t bytes,
	uint32_t *outcome)
{
	(void)opaque;
	(void)session;
	assert(held_spinlocks == 0U && bytes >= 4U);
	submit_runs++;
	memcpy(&submit_first_word, buffer, sizeof(submit_first_word));
	gpu_test_ticks += submit_ticks;
	*outcome = submit_outcome;
	return submit_error;
}

/* Sends one GPU_JOB_SUBMIT through the real dispatcher. */
static int
submit_call(
	struct test_file *file,
	int fd,
	uint64_t generation,
	const uint32_t *stream,
	uint32_t bytes,
	struct gpu_job_submit *request)
{
	memset(request, 0, sizeof(*request));
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->fd = fd;
	request->generation = generation;
	request->timeline = 1U;
	request->bytes = bytes;
	request->address = (uint64_t)(uintptr_t)stream;
	return fence_ioctl(file, GPU_JOB_SUBMIT, request);
}

/* Sends one GPU_JOB_RETIRE through the real dispatcher. */
static int
retire_call(
	struct test_file *file,
	struct gpu_job_retire_entry *entries,
	uint32_t count,
	uint32_t *retired)
{
	struct gpu_job_retire request;
	int error;

	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.count = count;
	request.address = (uint64_t)(uintptr_t)entries;
	error = fence_ioctl(file, GPU_JOB_RETIRE, &request);
	*retired = request.retired;
	return error;
}
