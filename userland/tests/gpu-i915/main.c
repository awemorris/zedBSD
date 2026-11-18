/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native i915 smoke test: copies, fills and stores through BCS0 batches on
 * /dev/gpu0 and observes one supervised job.
 *
 * The program speaks the raw GPU ioctls and the backend's native stream
 * (plan/ws029/i915-native-stream.md). It prints one START line, one PASS or
 * FAIL line, and the resource handles the remote harness matches against the
 * kernel log to check guest memory independently.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uapi/gpu.h>
#include <uapi/gpu-job.h>

/* MI_SEMAPHORE_WAIT with poll and greater-or-equal compare: never satisfied on a zero page. */
#define I915_MI_SEMAPHORE_WAIT_HANG	((0x1cU << 23) | 2U | (1U << 15) | (1U << 12))

/* Command words from the transcribed Linux definitions in src/drivers/gpu/i915/intel/commands.h. */
#define I915_MI_NOOP			0x00000000U
#define I915_MI_BATCH_BUFFER_END	0x05000000U
#define I915_MI_STORE_DWORD_IMM		0x10000002U
#define I915_XY_SRC_COPY_BLT		(0x40000000U | (0x53U << 22) | (3U << 20) | 8U)
#define I915_XY_COLOR_BLT		(0x40000000U | (0x50U << 22) | (3U << 20) | 5U)
#define I915_BLT_DEPTH_32		(3U << 24)
#define I915_BLT_ROP_SRC_COPY		(0xccU << 16)
#define I915_BLT_ROP_COLOR_COPY		(0xf0U << 16)

/* The native stream header (32 bytes) and relocation record (16 bytes). */
#define I915_STREAM_MAGIC		0x31394958U
#define I915_STREAM_ENGINE_BCS0		1U

/* Test image: 256 pixels of 4 bytes per row, 64 rows, 64 KiB per resource. */
#define TEST_WIDTH			256U
#define TEST_ROWS			64U
#define TEST_PITCH			(TEST_WIDTH * 4U)
#define TEST_BYTES			(TEST_PITCH * TEST_ROWS)
#define TEST_WORDS			(TEST_BYTES / 4U)
#define TEST_PATTERN_BASE		0x5a000000U
#define TEST_FILL_VALUE			0x3197a5e2U
#define TEST_STORE_VALUE		0xdeadbeefU
#define TEST_WAIT_NS			UINT64_C(10000000000)

/* The most recent stage, reported by the FAIL line. */
static const char *test_stage = "start";

/* Both resources and the working copies of their contents. */
static int test_fd = -1;
static uint64_t test_src;
static uint64_t test_dst;
static uint32_t test_source[TEST_WORDS];
static uint32_t test_readback[TEST_WORDS];

/* One stream buffer: header, up to two relocations and a short batch. */
static uint8_t test_stream[32U + 2U * 16U + 16U * 4U];

static int test_command(unsigned long command, void *request, const char *stage);
static int test_fail(int error);
static int test_info(void);
static int test_create(uint64_t *handle);
static int test_write(uint64_t handle, const uint32_t *words);
static int test_read(uint64_t handle, uint32_t *words);
static void test_put32(uint8_t *at, uint32_t value);
static uint32_t test_build(const uint32_t *batch, uint32_t dwords, uint32_t first_offset, uint64_t first, uint32_t second_offset, uint64_t second);
static int test_run(uint32_t bytes, const char *stage);
static int test_copy(void);
static int test_fill(void);
static int test_store(void);
static int test_job(void);
static int test_hang(void);
static int test_peer(void);

/*
 * Runs the four checks and prints the single result line; --hang runs the
 * isolation scenario instead.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	printf("GPUI915 START\n");
	fflush(stdout);

	/* The hang scenario forks a peer and drives the supervised recovery path. */
	if (argc > 1 && strcmp(argv[1], "--hang") == 0)
		return test_hang();

	/* The device must be the native backend with every capability the checks use. */
	test_fd = open("/dev/gpu0", O_RDWR);
	if (test_fd < 0)
		return test_fail(errno);
	error = test_info();
	if (error != 0)
		return test_fail(error);

	/* Source and destination are separate resources of the same size. */
	test_stage = "create-src";
	error = test_create(&test_src);
	if (error != 0)
		return test_fail(error);
	test_stage = "create-dst";
	error = test_create(&test_dst);
	if (error != 0)
		return test_fail(error);

	/* Each check leaves its evidence in the destination for the harness. */
	error = test_copy();
	if (error != 0)
		return test_fail(error);
	error = test_fill();
	if (error != 0)
		return test_fail(error);
	error = test_store();
	if (error != 0)
		return test_fail(error);
	error = test_job();
	if (error != 0)
		return test_fail(error);

	printf("GPUI915 PASS copy=1 fill=1 store=1 job=1 src_handle=%llu dst_handle=%llu\n",
	    (unsigned long long)test_src, (unsigned long long)test_dst);
	fflush(stdout);
	return 0;
}

/* Issues one ioctl and records the stage for a later failure report. */
static int
test_command(
	unsigned long command,
	void *request,
	const char *stage)
{
	int result;
	int error;

	test_stage = stage;
	result = ioctl(test_fd, command, request);
	error = errno;
	if (result != 0)
		return error;

	return 0;
}

/* Prints the failure line with the stage and errno and returns the exit status. */
static int
test_fail(
	int error)
{
	printf("GPUI915 FAIL stage=%s errno=%d\n", test_stage, error);
	fflush(stdout);
	return 1;
}

/* Checks the backend identity and capabilities. */
static int
test_info(void)
{
	struct gpu_info info;
	uint32_t needed;
	int error;

	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	error = test_command(GPU_GET_INFO, &info, "get-info");
	if (error != 0)
		return error;

	test_stage = "driver-name";
	if (strcmp(info.driver_name, "i915") != 0)
		return ENODEV;

	test_stage = "capabilities";
	needed = GPU_CAP_RESOURCE | GPU_CAP_TRANSFER | GPU_CAP_COMMAND | GPU_CAP_NOTIFICATION | GPU_CAP_JOB;
	if ((info.capabilities & needed) != needed)
		return ENOTSUP;

	return 0;
}

/* Creates one 64 KiB storage resource. */
static int
test_create(
	uint64_t *handle)
{
	struct gpu_resource_create request;
	int error;

	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.bytes = TEST_BYTES;
	request.usage = GPU_RESOURCE_USAGE_STORAGE;
	error = test_command(GPU_RESOURCE_CREATE, &request, test_stage);
	if (error != 0)
		return error;

	*handle = request.handle;
	return 0;
}

/* Uploads the whole working copy into a resource. */
static int
test_write(
	uint64_t handle,
	const uint32_t *words)
{
	struct gpu_transfer transfer;

	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = handle;
	transfer.address = (uint64_t)(uintptr_t)words;
	transfer.bytes = TEST_BYTES;
	return test_command(GPU_RESOURCE_WRITE, &transfer, "resource-write");
}

/* Downloads the whole resource into the working copy. */
static int
test_read(
	uint64_t handle,
	uint32_t *words)
{
	struct gpu_transfer transfer;

	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = handle;
	transfer.address = (uint64_t)(uintptr_t)words;
	transfer.bytes = TEST_BYTES;
	return test_command(GPU_RESOURCE_READ, &transfer, "resource-read");
}

/* Stores one little-endian word. */
static void
test_put32(
	uint8_t *at,
	uint32_t value)
{
	memcpy(at, &value, 4U);
}

/* Builds a BCS0 stream with up to two relocations; a zero handle means no relocation. */
static uint32_t
test_build(
	const uint32_t *batch,
	uint32_t dwords,
	uint32_t first_offset,
	uint64_t first,
	uint32_t second_offset,
	uint64_t second)
{
	uint32_t relocations;
	uint8_t *at;
	uint32_t index;

	relocations = 0U;
	if (first != 0U)
		relocations++;
	if (second != 0U)
		relocations++;

	memset(test_stream, 0, sizeof(test_stream));
	test_put32(test_stream, I915_STREAM_MAGIC);
	test_put32(test_stream + 4U, 1U);
	test_put32(test_stream + 8U, I915_STREAM_ENGINE_BCS0);
	test_put32(test_stream + 12U, relocations);
	test_put32(test_stream + 16U, dwords);

	/* Relocations follow the header in order. */
	at = test_stream + 32U;
	if (first != 0U) {
		test_put32(at, first_offset);
		memcpy(at + 8U, &first, 8U);
		at += 16U;
	}
	if (second != 0U) {
		test_put32(at, second_offset);
		memcpy(at + 8U, &second, 8U);
		at += 16U;
	}

	/* The batch closes the stream. */
	for (index = 0U; index < dwords; index++)
		test_put32(at + index * 4U, batch[index]);

	return 32U + relocations * 16U + dwords * 4U;
}

/* Submits the built stream and waits for its completion with success. */
static int
test_run(
	uint32_t bytes,
	const char *stage)
{
	struct gpu_command_submit submit;
	struct gpu_command_wait wait;
	int error;

	memset(&submit, 0, sizeof(submit));
	submit.version = GPU_ABI_VERSION;
	submit.size = sizeof(submit);
	submit.address = (uint64_t)(uintptr_t)test_stream;
	submit.bytes = bytes;
	error = test_command(GPU_COMMAND_SUBMIT, &submit, stage);
	if (error != 0)
		return error;

	/* The wait consumes the record; a non-zero status is the backend's error. */
	memset(&wait, 0, sizeof(wait));
	wait.version = GPU_ABI_VERSION;
	wait.size = sizeof(wait);
	wait.sequence = submit.sequence;
	wait.timeout_ns = TEST_WAIT_NS;
	wait.flags = GPU_WAIT_CONSUME;
	error = test_command(GPU_COMMAND_WAIT, &wait, stage);
	if (error != 0)
		return error;
	if (wait.status != 0U)
		return (int)wait.status;

	return 0;
}

/* Copies the patterned source into the destination and checks every word. */
static int
test_copy(void)
{
	uint32_t batch[11];
	uint32_t bytes;
	uint32_t index;
	int error;

	/* The pattern makes every word position distinguishable. */
	for (index = 0U; index < TEST_WORDS; index++)
		test_source[index] = TEST_PATTERN_BASE + index;
	error = test_write(test_src, test_source);
	if (error != 0)
		return error;

	batch[0] = I915_XY_SRC_COPY_BLT;
	batch[1] = I915_BLT_DEPTH_32 | I915_BLT_ROP_SRC_COPY | TEST_PITCH;
	batch[2] = 0U;
	batch[3] = (TEST_ROWS << 16) | TEST_WIDTH;
	batch[4] = 0U;
	batch[5] = 0U;
	batch[6] = 0U;
	batch[7] = TEST_PITCH;
	batch[8] = 0U;
	batch[9] = 0U;
	batch[10] = I915_MI_BATCH_BUFFER_END;
	bytes = test_build(batch, 11U, 4U, test_dst, 8U, test_src);
	error = test_run(bytes, "copy");
	if (error != 0)
		return error;

	error = test_read(test_dst, test_readback);
	if (error != 0)
		return error;
	test_stage = "copy-verify";
	if (memcmp(test_source, test_readback, TEST_BYTES) != 0)
		return EIO;

	return 0;
}

/* Fills the destination with one value and checks every word. */
static int
test_fill(void)
{
	uint32_t batch[9];
	uint32_t bytes;
	uint32_t index;
	int error;

	batch[0] = I915_XY_COLOR_BLT;
	batch[1] = I915_BLT_DEPTH_32 | I915_BLT_ROP_COLOR_COPY | TEST_PITCH;
	batch[2] = 0U;
	batch[3] = (TEST_ROWS << 16) | TEST_WIDTH;
	batch[4] = 0U;
	batch[5] = 0U;
	batch[6] = TEST_FILL_VALUE;
	batch[7] = I915_MI_NOOP;
	batch[8] = I915_MI_BATCH_BUFFER_END;
	bytes = test_build(batch, 9U, 4U, test_dst, 0U, 0U);
	error = test_run(bytes, "fill");
	if (error != 0)
		return error;

	error = test_read(test_dst, test_readback);
	if (error != 0)
		return error;
	test_stage = "fill-verify";
	for (index = 0U; index < TEST_WORDS; index++) {
		if (test_readback[index] != TEST_FILL_VALUE)
			return EIO;
	}

	return 0;
}

/* Stores one word at the start of the destination without touching the rest. */
static int
test_store(void)
{
	uint32_t batch[5];
	uint32_t bytes;
	int error;

	batch[0] = I915_MI_STORE_DWORD_IMM;
	batch[1] = 0U;
	batch[2] = 0U;
	batch[3] = TEST_STORE_VALUE;
	batch[4] = I915_MI_BATCH_BUFFER_END;
	bytes = test_build(batch, 5U, 1U, test_dst, 0U, 0U);
	error = test_run(bytes, "store");
	if (error != 0)
		return error;

	error = test_read(test_dst, test_readback);
	if (error != 0)
		return error;
	test_stage = "store-verify";
	if (test_readback[0] != TEST_STORE_VALUE || test_readback[1] != TEST_FILL_VALUE)
		return EIO;

	return 0;
}

/* Reserves, commits and waits for one supervised marker job on BCS0. */
static int
test_job(void)
{
	struct gpu_job_reserve reserve;
	struct gpu_job_action action;
	struct gpu_command_wait wait;
	int error;

	memset(&reserve, 0, sizeof(reserve));
	reserve.version = GPU_ABI_VERSION;
	reserve.size = sizeof(reserve);
	reserve.fd = -1;
	reserve.timeline = 1U;
	error = test_command(GPU_JOB_RESERVE, &reserve, "job-reserve");
	if (error != 0)
		return error;

	memset(&action, 0, sizeof(action));
	action.version = GPU_ABI_VERSION;
	action.size = sizeof(action);
	action.sequence = reserve.sequence;
	error = test_command(GPU_JOB_COMMIT, &action, "job-commit");
	if (error != 0)
		return error;

	memset(&wait, 0, sizeof(wait));
	wait.version = GPU_ABI_VERSION;
	wait.size = sizeof(wait);
	wait.sequence = reserve.sequence;
	wait.timeout_ns = TEST_WAIT_NS;
	wait.flags = GPU_WAIT_CONSUME;
	error = test_command(GPU_COMMAND_WAIT, &wait, "job-wait");
	if (error != 0)
		return error;
	test_stage = "job-status";
	if (wait.status != 0U)
		return (int)wait.status;

	return 0;
}

/*
 * Hangs BCS0 with a semaphore wait while a forked peer runs a copy; the
 * kernel's deadlines must end the hung job with an error and let the peer finish.
 */
static int
test_hang(void)
{
	struct gpu_command_submit submit;
	struct gpu_command_wait wait;
	uint32_t batch[5];
	uint32_t bytes;
	pid_t peer;
	int status;
	int error;

	/* The peer opens its own session before the hang is submitted. */
	peer = fork();
	if (peer < 0)
		return test_fail(errno);
	if (peer == 0)
		_exit(test_peer());

	/* The hung producer needs one resource for the semaphore address. */
	test_fd = open("/dev/gpu0", O_RDWR);
	if (test_fd < 0)
		return test_fail(errno);
	test_stage = "hang-create";
	error = test_create(&test_dst);
	if (error != 0)
		return test_fail(error);

	/* The batch waits for a value the zeroed resource never reaches. */
	batch[0] = I915_MI_SEMAPHORE_WAIT_HANG;
	batch[1] = 1U;
	batch[2] = 0U;
	batch[3] = 0U;
	batch[4] = I915_MI_BATCH_BUFFER_END;
	bytes = test_build(batch, 5U, 2U, test_dst, 0U, 0U);
	memset(&submit, 0, sizeof(submit));
	submit.version = GPU_ABI_VERSION;
	submit.size = sizeof(submit);
	submit.address = (uint64_t)(uintptr_t)test_stream;
	submit.bytes = bytes;
	error = test_command(GPU_COMMAND_SUBMIT, &submit, "hang-submit");
	if (error != 0)
		return test_fail(error);

	/* Supervision must turn the hang into a terminal error within the wait. */
	memset(&wait, 0, sizeof(wait));
	wait.version = GPU_ABI_VERSION;
	wait.size = sizeof(wait);
	wait.sequence = submit.sequence;
	wait.timeout_ns = UINT64_C(60000000000);
	wait.flags = GPU_WAIT_CONSUME;
	error = test_command(GPU_COMMAND_WAIT, &wait, "hang-wait");
	if (error != 0 && error != ENODEV && error != EIO)
		return test_fail(error);

	/* The peer's outcome is the evidence that isolation kept the engine usable. */
	status = -1;
	if (waitpid(peer, &status, 0) < 0)
		return test_fail(errno);
	printf("GPUI915 HANG wait_errno=%d status=%u peer=%s\n", error, wait.status,
	    (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? "ok" : "fail");
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		printf("GPUI915 ISOLATION_PEER_OK\n");
	fflush(stdout);
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}

/* The peer session waits for the hang to be queued, then runs one copy and verifies it. */
static int
test_peer(void)
{
	int error;

	sleep(2);
	test_fd = open("/dev/gpu0", O_RDWR);
	if (test_fd < 0)
		return 1;
	test_stage = "peer-create-src";
	error = test_create(&test_src);
	if (error != 0)
		return 1;
	test_stage = "peer-create-dst";
	error = test_create(&test_dst);
	if (error != 0)
		return 1;
	error = test_copy();
	printf("GPUI915 PEER copy=%s stage=%s errno=%d\n", error == 0 ? "ok" : "fail", test_stage, error);
	fflush(stdout);
	return error == 0 ? 0 : 1;
}
