/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises isolated Venus timeout containment and recovery through public GPU requests:
 * the device fails, a user's new open fails with EIO and starts no reset, root's open
 * takes only the reset, and root's GPU_DEVICE_RESET makes the device usable again.
 * Run as root.  The caller must stop every other GPU client before this destructive
 * device-loss test.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <uapi/gpu.h>

#define RECOVERY_BYTES 4096U
#define RECOVERY_WAIT_NS UINT64_C(15000000000)

/* The user a non-root open is made as: a member of the node's group (video), as on a text-console desktop. */
#define RECOVERY_USER_UID 1000U

/* One finite attempt owns every open whose retirement permits the checked reset. */
struct recovery_test {
	int primary;
	int peer;
	int fresh;
};

static int recovery_run(struct recovery_test *test);
static int recovery_command(int fd, unsigned long command, void *request, const char *stage);
static int recovery_time(uint64_t *milliseconds);
static int recovery_blob(int fd, struct gpu_blob_create *blob);
static int recovery_roundtrip(int fd, uint64_t handle);
static int recovery_decoder(int fd);
static int recovery_gate(const char *marker, const char *expected);
static int recovery_user_open(void);
static void recovery_failure(const char *stage, int result, int error);

/*
 * Runs only with explicit isolation, with a process alarm outside every finite kernel wait.
 */
int
main(
	int argc,
	char **argv)
{
	struct recovery_test test;
	int error;
	int result;

	/* Device failure affects independent opens, so ordinary invocation cannot start this test. */
	if (argc != 2)
		return 2;

	/* The dedicated harness runs this test only after all normal GPU users have retired. */
	error = strcmp(argv[1], "--isolated");
	if (error != 0)
		return 2;

	/* A finite alarm also covers unexpected stalls outside the command observation request. */
	alarm(45U);
	test.primary = -1;
	test.peer = -1;
	test.fresh = -1;
	puts("GPURECOVERY START isolated=1 watchdog_seconds=10 upper_seconds=45");
	fflush(stdout);

	/* Keep all acquired descriptions available for cleanup after any failed assertion. */
	result = recovery_run(&test);

	/* Ordinary close retires each remaining open; the harness owns the disposable VM afterward. */
	if (test.primary >= 0)
		close(test.primary);

	/* The independent peer may retain quarantined host-visible resource backing until reset. */
	if (test.peer >= 0)
		close(test.peer);

	/* The recovered session owns no resources once the successful roundtrip has completed. */
	if (test.fresh >= 0)
		close(test.fresh);

	/* No successful test leaves the process alarm active or a GPU open inherited by the shell. */
	alarm(0U);
	if (result != 0)
		return 1;

	/* Succeeded: finite timeout, old-session rejection and a new native context were observed. */
	puts("GPURECOVERY PASS timeout=1 peer_failed=1 user_open_eio=1 root_open_limited=1 root_reset=1 fresh_roundtrip=4096 decoder=1 user_open_after=1");
	fflush(stdout);
	return 0;
}

/*
 * Keeps two old descriptions alive across one missing native queue fence, then recovers through root's reset.
 */
static int
recovery_run(
	struct recovery_test *test)
{
	struct gpu_info info;
	struct gpu_blob_create old_blob;
	struct gpu_blob_create new_blob;
	struct gpu_command_submit submit;
	struct gpu_command_wait wait;
	struct gpu_resource_destroy destroy;
	struct gpu_transfer transfer;
	struct gpu_device_reset reset;
	struct pollfd descriptor;
	uint64_t started;
	uint64_t finished;
	uint8_t byte;
	int error;
	int result;

	/* Each independent open creates a Venus context without binding any native Vulkan queue. */
	test->primary = open("/dev/gpu0", O_RDWR);
	if (test->primary < 0) {
		recovery_failure("primary open", test->primary, errno);
		return -1;
	}

	/* Reject unrelated drivers whose completion requests do not use the paused Venus renderer. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	error = recovery_command(test->primary, GPU_GET_INFO, &info, "capabilities");
	if (error != 0)
		return -1;

	/* This host-specific fault scenario requires native Venus notifications and blob access. */
	result = strcmp(info.driver_name, "venus");
	if (result != 0 ||
	    (info.capabilities & GPU_CAP_NOTIFICATION) == 0U ||
	    (info.capabilities & GPU_CAP_BLOB) == 0U) {
		recovery_failure("Venus notification and blob capabilities", result, ENOTSUP);
		return -1;
	}

	/* A second open owns a published host blob whose backing cannot retire before reset. */
	test->peer = open("/dev/gpu0", O_RDWR);
	if (test->peer < 0) {
		recovery_failure("peer open", test->peer, errno);
		return -1;
	}

	/* Leave this allocation live while its independent producer context is lost. */
	error = recovery_blob(test->peer, &old_blob);
	if (error != 0)
		return -1;

	/* Let the isolated harness suspend this VM's renderer before native fence submission. */
	error = recovery_gate("GPURECOVERY READY renderer=running", "stopped\n");
	if (error != 0)
		return -1;

	/* Measure actual guest time only after the renderer has been suspended. */
	error = recovery_time(&started);
	if (error != 0)
		return -1;

	/* Decoder retirement requires the paused renderer to process this context fence. */
	memset(&submit, 0, sizeof(submit));
	submit.version = GPU_ABI_VERSION;
	submit.size = sizeof(submit);
	submit.flags = GPU_COMMAND_CONTEXT_FENCE;
	submit.timeline = 0U;
	error = recovery_command(test->primary, GPU_COMMAND_SUBMIT, &submit, "paused decoder submit");
	if (error != 0)
		return -1;

	/* The transport watchdog must make this accepted record terminal before the explicit observation limit. */
	memset(&wait, 0, sizeof(wait));
	wait.version = GPU_ABI_VERSION;
	wait.size = sizeof(wait);
	wait.sequence = submit.sequence;
	wait.timeout_ns = RECOVERY_WAIT_NS;
	wait.flags = GPU_WAIT_CONSUME;
	error = recovery_command(test->primary, GPU_COMMAND_WAIT, &wait, "watchdog observation");
	if (error != 0)
		return -1;

	/* Missing native retirement must be reported as transport timeout, never successful GPU completion. */
	if (wait.status != ETIMEDOUT) {
		recovery_failure("watchdog terminal status", (int)wait.status, ETIMEDOUT);
		return -1;
	}

	/* Verify the default watchdog elapsed in a bounded interval rather than an immediate decoder error. */
	error = recovery_time(&finished);
	if (error != 0)
		return -1;

	/* Guest scheduling may delay wakeup, but a fifteen-second observation cannot justify an unbounded wait. */
	if (finished < started ||
	    finished - started < 9000U ||
	    finished - started > 20000U) {
		recovery_failure("watchdog elapsed interval", -1, ETIMEDOUT);
		return -1;
	}

	/* Publish the measured fault boundary before checking rejection and reset lifetime. */
	printf("GPURECOVERY TIMEOUT status=%u elapsed_ms=%llu\n", wait.status,
		(unsigned long long)(finished - started));
	fflush(stdout);

	/* A different old open must observe the device-wide error through ordinary fd polling. */
	memset(&descriptor, 0, sizeof(descriptor));
	descriptor.fd = test->peer;
	descriptor.events = POLLIN;
	result = poll(&descriptor, 1U, 0);
	if (result != 1 || (descriptor.revents & POLLERR) == 0) {
		recovery_failure("peer error readiness", result, errno);
		return -1;
	}

	/* Copied access preserves the same sticky timeout without touching any stale host aperture. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = old_blob.handle;
	transfer.address = (uint64_t)(uintptr_t)&byte;
	transfer.bytes = sizeof(byte);
	result = ioctl(test->peer, GPU_RESOURCE_READ, &transfer);
	error = errno;
	if (result != -1 || error != (int)wait.status) {
		recovery_failure("peer resource rejected", result, error);
		return -1;
	}

	/* Closing the failed producer leaves an independent old session. */
	result = close(test->primary);
	test->primary = -1;
	if (result != 0) {
		recovery_failure("primary retirement", result, errno);
		return -1;
	}

	/* A user's new open of the failed device fails with EIO and starts no reset. */
	error = recovery_user_open();
	if (error != EIO) {
		recovery_failure("user open of the failed device", -1, error);
		return -1;
	}

	/* The last old session goes too; its uncertain backing stays until the reset. */
	result = close(test->peer);
	test->peer = -1;
	if (result != 0) {
		recovery_failure("peer retirement", result, errno);
		return -1;
	}

	/* With no old open left, a user's open still fails: opening never resets the device. */
	error = recovery_user_open();
	if (error != EIO) {
		recovery_failure("user open with no old open", -1, error);
		return -1;
	}
	puts("GPURECOVERY USER_OPEN errno=5");
	fflush(stdout);

	/* Resume the same owned renderer processes before asking hardware to create a replacement context. */
	error = recovery_gate("GPURECOVERY RESUME renderer=stopped", "running\n");
	if (error != 0)
		return -1;

	/* Root's open of the failed device succeeds but takes no ordinary request. */
	test->fresh = open("/dev/gpu0", O_RDWR);
	if (test->fresh < 0) {
		recovery_failure("root open of the failed device", test->fresh, errno);
		return -1;
	}
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	result = ioctl(test->fresh, GPU_GET_INFO, &info);
	error = errno;
	if (result != -1 || error != EIO) {
		recovery_failure("root open limited to the reset", result, error);
		return -1;
	}

	/* Root's reset: the driver's checked reset, then the open has a new native context. */
	memset(&reset, 0, sizeof(reset));
	reset.version = GPU_ABI_VERSION;
	reset.size = sizeof(reset);
	error = recovery_command(test->fresh, GPU_DEVICE_RESET, &reset, "root reset");
	if (error != 0)
		return -1;
	if (reset.result != 0 || reset.lost != 0U || reset.resets == 0U) {
		recovery_failure("root reset answer", reset.result, (int)reset.lost);
		return -1;
	}
	printf("GPURECOVERY RESET result=%d ms=%u resets=%u lost=%u\n", reset.result,
		reset.milliseconds, reset.resets, reset.lost);
	fflush(stdout);

	/* Recreate actual host-visible backing rather than testing only an immutable capability query. */
	error = recovery_blob(test->fresh, &new_blob);
	if (error != 0)
		return -1;

	/* Every byte traverses the restored driver mapping in both directions. */
	error = recovery_roundtrip(test->fresh, new_blob.handle);
	if (error != 0)
		return -1;

	/* A successful decoder fence proves new queued native work retires after recovery. */
	error = recovery_decoder(test->fresh);
	if (error != 0)
		return -1;

	/* Explicit destruction checks restored unmap, detach and host resource retirement. */
	memset(&destroy, 0, sizeof(destroy));
	destroy.version = GPU_ABI_VERSION;
	destroy.size = sizeof(destroy);
	destroy.handle = new_blob.handle;
	error = recovery_command(test->fresh, GPU_RESOURCE_DESTROY, &destroy, "fresh blob destroy");
	if (error != 0)
		return -1;

	/* A user opens the recovered device again. */
	error = recovery_user_open();
	if (error != 0) {
		recovery_failure("user open after the reset", -1, error);
		return -1;
	}

	/* Succeeded: replacement hardware state supports resources and independent command notification. */
	return 0;
}

/*
 * Opens /dev/gpu0 in a child process as a user of the video group, as a
 * desktop started from the text console does, and closes it.  Returns 0
 * when the open succeeds, its errno when it fails, or -1 when the child
 * could not be made that user.
 */
static int
recovery_user_open(
	void)
{
	struct group *video;
	gid_t group;
	pid_t child;
	pid_t waited;
	int status;
	int fd;
	int error;

	/* The node's group. */
	video = getgrnam("video");
	if (video == NULL)
		return -1;
	group = video->gr_gid;

	/* The child drops root before it opens. */
	fflush(stdout);
	child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		if (setgroups(1U, &group) != 0 || setgid(group) != 0 || setuid(RECOVERY_USER_UID) != 0)
			_exit(255);
		fd = open("/dev/gpu0", O_RDWR);
		if (fd < 0)
			_exit(errno & 127);
		close(fd);
		_exit(0);
	}

	/* The child's answer. */
	waited = waitpid(child, &status, 0);
	if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) == 255)
		return -1;
	error = WEXITSTATUS(status);

	/* Succeeded: the open's result. */
	return error;
}

/*
 * Reports the first failed real ioctl without changing its errno or hiding the failing operation.
 */
static int
recovery_command(
	int fd,
	unsigned long command,
	void *request,
	const char *stage)
{
	int result;
	int error;

	/* Capture errno before diagnostic output can issue additional system calls. */
	result = ioctl(fd, command, request);
	error = errno;
	if (result != 0) {
		recovery_failure(stage, result, error);
		return -1;
	}

	/* Succeeded: the caller may inspect the complete returned fixed-width request. */
	return 0;
}

/*
 * Converts the guest monotonic clock into the finite watchdog's millisecond interval.
 */
static int
recovery_time(
	uint64_t *milliseconds)
{
	struct timespec now;
	int error;

	/* A real guest clock distinguishes an elapsed watchdog from immediate host protocol rejection. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0) {
		recovery_failure("monotonic time", error, errno);
		return -1;
	}

	/* The test runs for seconds and cannot overflow a monotonic 64-bit millisecond counter. */
	*milliseconds = (uint64_t)now.tv_sec * 1000U;
	*milliseconds += (uint64_t)now.tv_nsec / 1000000U;

	/* Succeeded: one independent clock sample is ready for bounded subtraction. */
	return 0;
}

/*
 * Allocates native reply storage without introducing Vulkan queue or device-memory identities.
 */
static int
recovery_blob(
	int fd,
	struct gpu_blob_create *blob)
{
	int error;

	/* Blob zero is a private mapped allocation belonging to this independently opened context. */
	memset(blob, 0, sizeof(*blob));
	blob->version = GPU_ABI_VERSION;
	blob->size = sizeof(*blob);
	blob->bytes = RECOVERY_BYTES;
	blob->flags = GPU_BLOB_MAPPABLE;
	error = recovery_command(fd, GPU_BLOB_CREATE, blob, "native blob create");
	if (error != 0)
		return -1;

	/* Succeeded: close or explicit destroy owns the allocation's complete native lifetime. */
	return 0;
}

/*
 * Checks the recovered host-visible allocation with a deterministic full-page byte pattern.
 */
static int
recovery_roundtrip(
	int fd,
	uint64_t handle)
{
	struct gpu_transfer transfer;
	uint8_t written[RECOVERY_BYTES];
	uint8_t observed[RECOVERY_BYTES];
	unsigned index;
	int error;

	/* A position-dependent pattern catches stale, aliased and partially accessible backing. */
	for (index = 0U; index < RECOVERY_BYTES; index++)
		written[index] = (uint8_t)((index * 37U + 19U) & 255U);

	/* Publish every expected byte through the ordinary bounded copied-access operation. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = handle;
	transfer.address = (uint64_t)(uintptr_t)written;
	transfer.bytes = sizeof(written);
	error = recovery_command(fd, GPU_RESOURCE_WRITE, &transfer, "fresh blob write");
	if (error != 0)
		return -1;

	/* Read into independent storage so a no-op implementation cannot preserve the expected pattern. */
	memset(observed, 0, sizeof(observed));
	transfer.address = (uint64_t)(uintptr_t)observed;
	error = recovery_command(fd, GPU_RESOURCE_READ, &transfer, "fresh blob read");
	if (error != 0)
		return -1;

	/* Compare the entire allocation rather than a single sentinel value. */
	error = memcmp(written, observed, sizeof(written));
	if (error != 0) {
		recovery_failure("fresh blob contents", error, EIO);
		return -1;
	}

	/* Succeeded: all recovered allocation bytes match the independently generated pattern. */
	return 0;
}

/*
 * Observes a valid decoder timeline after recovery without claiming native GPU execution completion.
 */
static int
recovery_decoder(
	int fd)
{
	struct gpu_command_submit submit;
	struct gpu_command_wait wait;
	int error;

	/* Timeline zero needs no VkQueue binding and fences only decoder-stream processing. */
	memset(&submit, 0, sizeof(submit));
	submit.version = GPU_ABI_VERSION;
	submit.size = sizeof(submit);
	submit.flags = GPU_COMMAND_CONTEXT_FENCE;
	error = recovery_command(fd, GPU_COMMAND_SUBMIT, &submit, "fresh decoder submit");
	if (error != 0)
		return -1;

	/* The replacement queue must deliver its exact retained completion within a finite interval. */
	memset(&wait, 0, sizeof(wait));
	wait.version = GPU_ABI_VERSION;
	wait.size = sizeof(wait);
	wait.sequence = submit.sequence;
	wait.timeout_ns = RECOVERY_WAIT_NS;
	wait.flags = GPU_WAIT_CONSUME;
	error = recovery_command(fd, GPU_COMMAND_WAIT, &wait, "fresh decoder wait");
	if (error != 0)
		return -1;

	/* Any transport error here means the replacement context has not recovered. */
	if (wait.status != 0U) {
		recovery_failure("fresh decoder terminal status", (int)wait.status, EIO);
		return -1;
	}

	/* Succeeded: the new native decoder response returned through the restarted completion queue. */
	return 0;
}

/* Coordinates one explicit renderer state transition with the disposable VM harness. */
static int
recovery_gate(
	const char *marker,
	const char *expected)
{
	char input[16];
	char *line;
	int different;

	/* Publish the synchronization point before blocking for a confirmed host transition. */
	puts(marker);
	fflush(stdout);
	line = fgets(input, sizeof(input), stdin);
	if (line == NULL) {
		recovery_failure("renderer coordination input", -1, EIO);
		return -1;
	}

	/* Only the expected acknowledgement permits the next native device operation. */
	different = strcmp(input, expected);
	if (different != 0) {
		recovery_failure("renderer coordination acknowledgement", -1, EINVAL);
		return -1;
	}

	/* Succeeded: the harness confirms the state required by the next test stage. */
	return 0;
}

/*
 * Preserves a compact failure marker in serial evidence before normal cleanup runs.
 */
static void
recovery_failure(
	const char *stage,
	int result,
	int error)
{
	/* The harness distinguishes a failed check from loader failure or a bounded external timeout. */
	printf("GPURECOVERY FAIL stage=%s result=%d errno=%d\n", stage, result, error);
	fflush(stdout);

	/* Succeeded: the first failed boundary is visible in the disposable VM's console log. */
	return;
}
