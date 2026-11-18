/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests standard OPAQUE_FD fence sharing between independent Vulkan processes.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define FENCE_TEST_WAIT UINT64_C(10000000000)
#define FENCE_TEST_EXIT_WAIT UINT64_C(120000000000)
#define FENCE_LOAD_SUBMITS 96U
#define FENCE_LOAD_BYTES 4194304U
#define FENCE_LOAD_REPEATS 32U
#define FENCE_LOAD_PROOF_BYTES (FENCE_LOAD_SUBMITS * 4U)
#define FENCE_LOAD_PROOF_BASE 0x73190000U

/* Each process owns an independent instance, device and real GPU fill recording. */
struct fence_test_context {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkFence shared;
	VkFence private_fence;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkBuffer proof_buffer;
	VkDeviceMemory proof_memory;
	VkCommandBuffer proof_commands[FENCE_LOAD_SUBMITS];
	VkCommandPool pool;
	VkCommandBuffer command;
	VkEvent event;
	uint32_t family;
};

/* Each forked process reports the most recent public operation when a check fails. */
static const char *test_role = "startup";
static const char *test_stage = "entry";
static VkResult test_vulkan_result;

static void test_failure(unsigned line);
static int context_create(struct fence_test_context *context);
static void context_finish(struct fence_test_context *context);
static int create_fence(struct fence_test_context *context, VkBool32 signaled, VkBool32 exportable, VkFence *fence);
static int create_work(struct fence_test_context *context, int blocked);
static int create_work_sized(struct fence_test_context *context, int blocked, VkDeviceSize bytes, unsigned repeats, int simultaneous);
static int delay_process(int socket, int producer, int timeout);
static int delay_producer(struct fence_test_context *context, int socket, int timeout);
static int delay_peer(struct fence_test_context *context, int socket);
static int delay_peer_work(struct fence_test_context *context);
static int delay_gate(const char *marker, const char *expected);
static int load_process(int socket);
static int load_round(struct fence_test_context *context, int socket, VkFence *fences, unsigned round);
static int load_proof_create(struct fence_test_context *context);
static int load_proof_clear(struct fence_test_context *context);
static int load_proof_verify(struct fence_test_context *context);
static int load_cpu(uint64_t *user, uint64_t *system);
static int load_clear(struct fence_test_context *context);
static int load_verify(struct fence_test_context *context);
static int submit_work(struct fence_test_context *context);
static int send_byte(int socket, char value);
static int receive_byte(int socket, char expected);
static int send_fence(int socket, struct fence_test_context *context, VkFence fence);
static int receive_fence(int socket, struct fence_test_context *context, VkFence fence, VkFenceImportFlags flags);
static int parent_test(int socket);
static int child_test(int socket);
static int exit_producer(int socket, int stop, int delayed);
static int exit_consumer(int socket, pid_t stopped_producer, int delayed, int hang);
static int consumer_work(struct fence_test_context *context);
static int test_clock(uint64_t *nanoseconds);
static int check_result(VkResult actual, VkResult expected, const char *operation);

/*
 * Runs ordinary lifetime checks or one isolated producer-lifetime fault scenario.
 */
int
main(
	int argc,
	char **argv)
{
	int sockets[2];
	int producer_exit;
	int producer_stop;
	int producer_delayed;
	int producer_hang;
	int submit_load;
	int completion_delay;
	int context_timeout;
	int error;
	int status;
	int child_status;
	pid_t child;
	pid_t waited;

	/* Explicit modes select either normal concurrent work or one isolated lifetime regression. */
	producer_exit = 0;
	producer_stop = 0;
	producer_delayed = 0;
	producer_hang = 0;
	submit_load = 0;
	completion_delay = 0;
	context_timeout = 0;
	if (argc == 2) {
		error = strcmp(argv[1], "--producer-exit");
		if (error == 0)
			producer_exit = 1;

		/* A stopped producer retains a valid job whose host completion notification is delayed. */
		error = strcmp(argv[1], "--producer-stop");
		if (error == 0) {
			producer_exit = 1;
			producer_stop = 1;
		}

		/* An exited producer's committed finite job, delayed by the host, must still complete for its consumer. */
		error = strcmp(argv[1], "--producer-exit-delayed");
		if (error == 0) {
			producer_exit = 1;
			producer_delayed = 1;
		}

		/* An exited producer's hung job must fail by deadline and isolate only that context. */
		error = strcmp(argv[1], "--producer-exit-hang");
		if (error == 0) {
			producer_exit = 1;
			producer_hang = 1;
		}

		/* This finite workload uses ordinary public API calls across independent processes. */
		error = strcmp(argv[1], "--submit-load");
		if (error == 0)
			submit_load = 1;

		/* The host delays completion notification after a real native fence succeeds. */
		error = strcmp(argv[1], "--completion-delay");
		if (error == 0)
			completion_delay = 1;

		/* The same isolated host delay is tested against a shorter administrator-selected deadline. */
		error = strcmp(argv[1], "--context-timeout");
		if (error == 0) {
			completion_delay = 1;
			context_timeout = 1;
		}

		/* An unknown mode must not silently execute ordinary acceptance. */
		if (producer_exit == 0 &&
		    submit_load == 0 &&
		    completion_delay == 0)
			return 2;
	} else if (argc != 1) {
		return 2;
	}

	/* An early marker distinguishes loader failure from a failure in this standard API test. */
	puts("GPUFENCE START");
	fflush(stdout);

	/* Fork before opening either Vulkan instance, so neither process inherits a GPU session. */
	test_stage = "socketpair";
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Neither Vulkan session exists yet, so the processes cannot accidentally share one native context. */
	test_stage = "fork";
	child = fork();
	if (child < 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The child owns only its socket endpoint and selected GPU workload. */
	if (child == 0) {
		test_role = "child";
		close(sockets[0]);
		if (completion_delay != 0) {
			status = delay_process(sockets[1], 1, context_timeout);
		} else if (submit_load != 0) {
			status = load_process(sockets[1]);
		} else if (producer_exit) {
			status = exit_producer(sockets[1], producer_stop, producer_delayed);
		} else {
			status = child_test(sockets[1]);
		}

		/* Retire the private channel before reporting this process's independent result. */
		close(sockets[1]);
		fflush(NULL);
		_exit(status);
	}

	/* The parent reports success only after both independent processes finish their checks. */
	test_role = "parent";
	close(sockets[1]);
	if (completion_delay != 0) {
		status = delay_process(sockets[0], 0, context_timeout);
	} else if (submit_load != 0) {
		status = load_process(sockets[0]);
	} else if (producer_exit) {
		/* The stop scenario requires waitpid proof before observing the still-live shared payload. */
		if (producer_stop != 0)
			status = exit_consumer(sockets[0], child, 0, 0);
		else
			status = exit_consumer(sockets[0], 0, producer_delayed, producer_hang);
	} else {
		status = parent_test(sockets[0]);
	}

	/* The workload has finished exchanging every required peer proof. */
	close(sockets[0]);

	/* A failed stop scenario must not strand a stopped child before the final bounded harness cleanup. */
	if (producer_stop != 0 && status != 0)
		(void)kill(child, SIGKILL);

	/* No success marker precedes the final child exit status. */
	waited = waitpid(child, &child_status, 0);
	if (waited != child ||
	    !WIFEXITED(child_status) ||
	    WEXITSTATUS(child_status) != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* A successful child cannot hide a failed parent workload. */
	if (status != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Publish ordinary acceptance only after peer completion and resource cleanup. */
	puts("GPUFENCE PASS");

	/* A fault scenario has its own marker so the host cannot accept ordinary coverage in its place. */
	if (producer_stop != 0)
		puts("GPUFENCE PRODUCER_STOP_ERROR PASS");
	else if (producer_delayed != 0)
		puts("GPUFENCE PRODUCER_EXIT_DELAYED PASS");
	else if (producer_hang != 0)
		puts("GPUFENCE PRODUCER_EXIT_HANG PASS");
	else if (producer_exit)
		puts("GPUFENCE PRODUCER_EXIT_REAL_RESULT PASS");
	else if (submit_load != 0)
		puts("GPUFENCE SUBMIT_LOAD PASS processes=2 rounds=3 submits=576 verified_bytes=25165824 verified_submits=576");

	/* The host delay tests require independent progress and the child's completed resource cleanup. */
	if (completion_delay != 0) {
		if (context_timeout != 0)
			puts("GPUFENCE CONTEXT_TIMEOUT PASS");
		else
			puts("GPUFENCE COMPLETION_DELAY PASS");
	}

	/* Succeeded: both processes completed the requested ordinary or isolated fault scenario. */
	return 0;
}

/* Owns one independent device throughout the isolated notification-delay regression. */
static int
delay_process(
	int socket,
	int producer,
	int timeout)
{
	struct fence_test_context context;
	int error;

	/* Partial setup is retained until the common cleanup, including device-loss paths. */
	memset(&context, 0, sizeof(context));
	error = context_create(&context);
	if (error != 0)
		goto finished;

	/* These recordings perform finite GPU writes without any application-controlled dependency. */
	error = create_work_sized(&context, 0, FENCE_LOAD_BYTES, 1U, 1);
	if (error != 0)
		goto finished;

	/* Exportable payloads exercise the authoritative kernel completion path. */
	error = create_fence(&context, VK_FALSE, VK_TRUE, &context.shared);
	if (error != 0)
		goto finished;

	/* The child is the sole delayed producer; the parent continuously exercises an independent context. */
	if (producer != 0)
		error = delay_producer(&context, socket, timeout);
	else
		error = delay_peer(&context, socket);

finished:
	/* Native cleanup may outlive logical device loss until the delayed callback actually retires. */
	context_finish(&context);
	if (error != 0)
		return error;

	/* The peer keeps its own device alive until this close has completed. */
	if (producer != 0) {
		error = send_byte(socket, 'D');
		if (error != 0)
			return error;
	}

	/* Succeeded: no process-owned session is left outside final main-level waitpid. */
	return 0;
}

/* Waits for one delayed authoritative callback, with normal and shortened-policy expectations. */
static int
delay_producer(
	struct fence_test_context *context,
	int socket,
	int timeout)
{
	VkResult status;
	VkResult expected;
	uint64_t started;
	uint64_t ended;
	uint64_t elapsed;
	int error;

	/* Setup contains no GPU submissions that could consume the host's one-shot delay gate. */
	error = send_byte(socket, 'I');
	if (error != 0)
		return error;

	/* The parent waits for the host to arm the gate before allowing this sole initial submission. */
	error = receive_byte(socket, 'G');
	if (error != 0)
		return error;

	/* Clearing the result makes the normal completion oracle independent of old contents. */
	error = load_clear(context);
	if (error != 0)
		return error;

	/* Measure submission through authoritative completion, including the injected notification delay. */
	error = test_clock(&started);
	if (error != 0)
		return error;

	/* The isolated host delays notification only after a real native VkFence reports SUCCESS. */
	error = submit_work(context);
	if (error != 0)
		return error;

	/* The harness must observe the gate acquisition before permitting the independent peer to submit. */
	error = send_byte(socket, 'S');
	if (error != 0)
		return error;

	/* The timeout case uses an image configured for an eight-second execution deadline. */
	expected = VK_SUCCESS;
	if (timeout != 0)
		expected = VK_ERROR_DEVICE_LOST;

	/* No userspace worker supplies the completion being observed by this API call. */
	test_stage = "delay vkWaitForFences";
	status = vkWaitForFences(context->device, 1U, &context->shared, VK_TRUE, FENCE_TEST_EXIT_WAIT);
	test_vulkan_result = status;
	error = test_clock(&ended);
	if (error != 0)
		return error;

	/* A host-side gate marker alone is insufficient without the matching guest result and interval. */
	elapsed = (ended - started) / UINT64_C(1000000);
	printf("GPUFENCE DELAY_RESULT result=%d expected=%d elapsed_ms=%llu\n", status, expected, (unsigned long long)elapsed);
	fflush(stdout);
	error = check_result(status, expected, "isolated delayed completion result");
	if (error != 0)
		return error;

	/* Logical failure must precede the fifteen-second delayed callback in the shortened-policy image. */
	if (timeout != 0) {
		if (elapsed < 7000U || elapsed > 13000U) {
			test_failure(__LINE__);
			return 1;
		}
	} else {
		/* The normal policy must tolerate an actual outstanding job notification beyond ten seconds. */
		if (elapsed < 14000U || elapsed > 30000U) {
			test_failure(__LINE__);
			return 1;
		}

		/* Only a healthy device may be mapped for diagnostic readback. */
		error = load_verify(context);
		if (error != 0)
			return error;
	}

	/* Succeeded: logical completion matched the configured common supervision policy. */
	return 0;
}

/* Continues real work on a second context before, during, and after the delayed context closes. */
static int
delay_peer(
	struct fence_test_context *context,
	int socket)
{
	struct timespec interval;
	VkResult status;
	uint64_t started;
	uint64_t now;
	unsigned completed;
	int error;

	/* Both devices exist before the host arms its one-shot gate. */
	error = receive_byte(socket, 'I');
	if (error != 0)
		return error;

	/* Only the isolated test harness can grant the first submission after its gate is ready. */
	error = delay_gate("GPUFENCE DELAY_READY", "armed\n");
	if (error != 0)
		return error;

	/* The child alone submits before the host identifies the delayed renderer context. */
	error = send_byte(socket, 'G');
	if (error != 0)
		return error;

	/* Native submission acceptance precedes the harness's callback-gate observation. */
	error = receive_byte(socket, 'S');
	if (error != 0)
		return error;

	/* The second handshake excludes accidentally delaying this independent peer instead. */
	error = delay_gate("GPUFENCE DELAY_SUBMITTED", "running\n");
	if (error != 0)
		return error;

	/* Twenty seconds spans both the shortened execution deadline and actual delayed retirement. */
	error = test_clock(&started);
	if (error != 0)
		return error;

	completed = 0U;
	interval.tv_sec = 0;
	interval.tv_nsec = 100000000L;
	for (;;) {
		/* Every iteration produces and verifies fresh bytes through the independent logical device. */
		error = delay_peer_work(context);
		if (error != 0)
			return error;

		/* Progress must continue after the other context's eight-second logical failure. */
		completed++;
		error = test_clock(&now);
		if (error != 0)
			return error;

		/* Periodic evidence certifies real completed work rather than a live sleeping process. */
		if (completed == 1U || completed % 25U == 0U) {
			printf("GPUFENCE DELAY_PEER completed=%u elapsed_ms=%llu\n", completed, (unsigned long long)((now - started) / UINT64_C(1000000)));
			fflush(stdout);
		}

		/* A finite observation window ends only after a successful operation beyond twenty seconds. */
		if (now - started >= UINT64_C(20000000000))
			break;

		/* The test deliberately gives the other process and renderer time to progress. */
		error = nanosleep(&interval, NULL);
		if (error != 0)
			return 1;
	}

	/* The delayed process must finish native resource destruction before the last independent submit. */
	error = receive_byte(socket, 'D');
	if (error != 0)
		return error;

	/* This new work cannot have completed before the other process's close was acknowledged. */
	error = delay_peer_work(context);
	if (error != 0)
		return error;

	/* A final queue observation rejects device-wide failure hidden by earlier successful submissions. */
	status = vkQueueWaitIdle(context->queue);
	error = check_result(status, VK_SUCCESS, "independent context survives producer close");
	if (error != 0)
		return error;

	/* Publish exact completion evidence for the outer isolated-VM verifier. */
	printf("GPUFENCE DELAY_PEER_PASS completed=%u elapsed_ms=%llu after_close=1 verified_bytes=%u\n", completed + 1U, (unsigned long long)((now - started) / UINT64_C(1000000)), FENCE_LOAD_BYTES);
	fflush(stdout);

	/* Succeeded: independent work and mapping remained usable across the other context's lifetime. */
	return 0;
}

/* Performs one fresh, finite transfer on the continuously running peer context. */
static int
delay_peer_work(
	struct fence_test_context *context)
{
	VkResult status;
	int error;

	/* The preceding iteration always finished before reusing this explicit fence. */
	status = vkResetFences(context->device, 1U, &context->shared);
	error = check_result(status, VK_SUCCESS, "independent peer reset");
	if (error != 0)
		return error;

	/* A previous round's matching bytes cannot satisfy this round's result check. */
	error = load_clear(context);
	if (error != 0)
		return error;

	/* Completion is tied to the real native queue and its authoritative callback. */
	error = submit_work(context);
	if (error != 0)
		return error;

	/* The independent context should progress promptly even while the other callback is delayed. */
	status = vkWaitForFences(context->device, 1U, &context->shared, VK_TRUE, FENCE_TEST_WAIT);
	error = check_result(status, VK_SUCCESS, "independent peer completion");
	if (error != 0)
		return error;

	/* Verify the actual transfer result after every successful completion. */
	error = load_verify(context);
	if (error != 0)
		return error;

	/* Succeeded: one fresh independent job completed and wrote the expected bytes. */
	return 0;
}

/* Synchronizes an isolated host operation with a precise guest submission boundary. */
static int
delay_gate(
	const char *marker,
	const char *expected)
{
	char input[32];
	char *line;
	int error;

	/* The host observes the marker before making its one bounded external operation. */
	puts(marker);
	fflush(stdout);
	line = fgets(input, sizeof(input), stdin);
	if (line == NULL)
		return 1;

	/* An unrelated terminal line must not accidentally advance the regression. */
	error = strcmp(input, expected);
	if (error != 0)
		return 1;

	/* Succeeded: the host explicitly acknowledged this boundary. */
	return 0;
}

/* Runs three measured rounds while a peer owns a separate logical GPU device. */
static int
load_process(
	int socket)
{
	struct fence_test_context context;
	VkFence fences[FENCE_LOAD_SUBMITS];
	unsigned created;
	unsigned index;
	unsigned round;
	int error;

	/* A partial setup retains every native object for the common cleanup below. */
	memset(&context, 0, sizeof(context));
	memset(fences, 0, sizeof(fences));
	created = 0U;
	error = context_create(&context);
	if (error != 0)
		goto finished;

	/* Repeated fills create a finite normal GPU workload without an unsignaled host dependency. */
	error = create_work_sized(&context, 0, FENCE_LOAD_BYTES, FENCE_LOAD_REPEATS, 1);
	if (error != 0)
		goto finished;

	/* Distinct proof recordings detect missing accepted work independently from the repeated fill. */
	error = load_proof_create(&context);
	if (error != 0)
		goto finished;

	/* Explicit-fence control work has the same submission count as the private-fence rounds. */
	for (index = 0U; index < FENCE_LOAD_SUBMITS; index++) {
		error = create_fence(&context, VK_FALSE, VK_FALSE, &fences[index]);
		if (error != 0)
			goto finished;

		/* Only created objects belong to the final destruction loop. */
		created++;
	}

	/* The final private-fence round demonstrates cache reuse after the explicit-fence control. */
	for (round = 0U; round < 3U; round++) {
		error = load_round(&context, socket, fences, round);
		if (error != 0)
			goto finished;
	}

finished:
	/* Completion or device loss must retire GPU use before the native fence handles are destroyed. */
	if (context.device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(context.device);
		for (index = 0U; index < created; index++) {
			/* The explicit-fence array belongs only to this independent process. */
			vkDestroyFence(context.device, fences[index], NULL);
		}
	}

	/* Device teardown also retains partial buffer and command-pool allocations. */
	context_finish(&context);
	if (error != 0)
		return error;

	/* Succeeded: each workload returned normally and its GPU bytes matched the expected pattern. */
	return 0;
}

/* Creates immutable per-submit proof recordings outside the measured workload window. */
static int
load_proof_create(
	struct fence_test_context *context)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocation;
	VkCommandBufferAllocateInfo commands;
	VkCommandBufferBeginInfo begin;
	VkMemoryBarrier barrier;
	VkResult status;
	uint32_t type;
	uint32_t attributes;
	unsigned index;

	/* Each submission owns one separate word, so a later write cannot hide missing work. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = FENCE_LOAD_PROOF_BYTES;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	test_stage = "proof vkCreateBuffer";
	status = vkCreateBuffer(context->device, &buffer, NULL, &context->proof_buffer);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Only memory supported by this proof buffer can be selected for its binding. */
	vkGetBufferMemoryRequirements(context->device, context->proof_buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(context->physical, &properties);
	for (type = 0U; type < 32U; type++) {
		/* Native requirement bits alone do not establish CPU mapping support. */
		if ((requirements.memoryTypeBits & (1U << type)) == 0U)
			continue;

		/* The property array has its own bound even when the requirement mask is wider. */
		if (type >= properties.memoryTypeCount)
			continue;

		/* CPU clear and verification require directly mapped storage. */
		attributes = properties.memoryTypes[type].propertyFlags;
		if ((attributes & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
			continue;

		/* Coherent storage avoids accepting data that needs a missing cache invalidation. */
		if ((attributes & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U)
			continue;

		/* This type satisfies both the native buffer and its CPU oracle. */
		break;
	}

	/* An unsupported proof allocation must fail this acceptance workload explicitly. */
	if (type == 32U) {
		test_failure(__LINE__);
		return 1;
	}

	/* Allocation retains the native alignment and size returned for this exact buffer. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;
	test_stage = "proof vkAllocateMemory";
	status = vkAllocateMemory(context->device, &allocation, NULL, &context->proof_memory);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Binding completes before any immutable proof command is recorded. */
	test_stage = "proof vkBindBufferMemory";
	status = vkBindBufferMemory(context->device, context->proof_buffer, context->proof_memory, 0U);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* The existing pool owns all proof recordings through partial setup and final cleanup. */
	memset(&commands, 0, sizeof(commands));
	commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commands.commandPool = context->pool;
	commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commands.commandBufferCount = FENCE_LOAD_SUBMITS;
	test_stage = "proof vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(context->device, &commands, context->proof_commands);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* A proof recording is reused only after the preceding round's queue-idle barrier. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	/* Every completed proof write becomes available to the round's host verification. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

	/* Each submit's distinct recording writes only its immutable slot and value. */
	for (index = 0U; index < FENCE_LOAD_SUBMITS; index++) {
		/* A failed recording cannot enter the measured submission window. */
		test_stage = "proof vkBeginCommandBuffer";
		status = vkBeginCommandBuffer(context->proof_commands[index], &begin);
		test_vulkan_result = status;
		if (status != VK_SUCCESS) {
			test_failure(__LINE__);
			return 1;
		}

		/* Missing submissions and incorrectly repeated identities leave a wrong word. */
		vkCmdFillBuffer(
			context->proof_commands[index],
			context->proof_buffer,
			(VkDeviceSize)index * sizeof(uint32_t),
			sizeof(uint32_t),
			FENCE_LOAD_PROOF_BASE + index + 1U);

		/* The CPU oracle runs only after queue-idle and this transfer-to-host dependency. */
		vkCmdPipelineBarrier(
			context->proof_commands[index],
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT,
			0U,
			1U,
			&barrier,
			0U,
			NULL,
			0U,
			NULL);

		/* Only a completely recorded immutable proof can be paired with the heavy command. */
		test_stage = "proof vkEndCommandBuffer";
		status = vkEndCommandBuffer(context->proof_commands[index]);
		test_vulkan_result = status;
		if (status != VK_SUCCESS) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* Succeeded: each measured submit now has a separately observable GPU effect. */
	return 0;
}

/* Clears every proof word after prior completion and before either measured producer starts. */
static int
load_proof_clear(
	struct fence_test_context *context)
{
	void *mapping;
	VkResult status;

	/* Coherent host writes make all missing submissions retain an unmistakable zero sentinel. */
	mapping = NULL;
	test_stage = "proof clear vkMapMemory";
	status = vkMapMemory(context->device, context->proof_memory, 0U, FENCE_LOAD_PROOF_BYTES, 0U, &mapping);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* No previous round's correct GPU word may satisfy this round's proof. */
	memset(mapping, 0, FENCE_LOAD_PROOF_BYTES);
	vkUnmapMemory(context->device, context->proof_memory);

	/* Succeeded: clearing remains outside the measured submission and completion interval. */
	return 0;
}

/* Checks every submit's distinct GPU-written word after the measured queue completion. */
static int
load_proof_verify(
	struct fence_test_context *context)
{
	const uint32_t *words;
	void *mapping;
	VkResult status;
	uint32_t expected;
	unsigned index;
	int error;

	/* The diagnostic map observes storage used by the real native proof commands. */
	mapping = NULL;
	test_stage = "proof verify vkMapMemory";
	status = vkMapMemory(context->device, context->proof_memory, 0U, FENCE_LOAD_PROOF_BYTES, 0U, &mapping);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Distinct slots prevent the last accepted submit from hiding a missing earlier one. */
	words = mapping;
	error = 0;
	for (index = 0U; index < FENCE_LOAD_SUBMITS; index++) {
		/* A repeated or omitted proof cannot match every index-dependent word. */
		expected = FENCE_LOAD_PROOF_BASE + index + 1U;
		if (words[index] != expected) {
			printf("GPUFENCE LOAD FAILED role=%s proof=%u actual=%x expected=%x\n", test_role, index, words[index], expected);
			error = 1;
			break;
		}
	}

	/* Diagnostic ownership ends even when a missing submission was detected. */
	vkUnmapMemory(context->device, context->proof_memory);
	if (error != 0)
		return error;

	/* Succeeded: all separately submitted proof commands wrote their own expected word. */
	return 0;
}

/* Measures one equal-sized submission window and checks its actual GPU output. */
static int
load_round(
	struct fence_test_context *context,
	int socket,
	VkFence *fences,
	unsigned round)
{
	VkSubmitInfo submit;
	VkCommandBuffer submitted_commands[2];
	VkFence fence;
	VkResult status;
	const char *mode;
	uint64_t started;
	uint64_t ended;
	uint64_t user_before;
	uint64_t system_before;
	uint64_t user_after;
	uint64_t system_after;
	unsigned index;
	int error;

	/* Each round starts with different bytes so stale results from an earlier round cannot pass. */
	error = load_clear(context);
	if (error != 0)
		return error;

	/* Every proof word is reset before measurement, after any previous round has retired. */
	error = load_proof_clear(context);
	if (error != 0)
		return error;

	/* Both processes finish the preceding round before either measured window begins. */
	error = send_byte(socket, 'L');
	if (error != 0)
		return error;

	/* A failed peer cannot be mistaken for an accepted start signal. */
	error = receive_byte(socket, 'L');
	if (error != 0)
		return error;

	/* The same command stream is used for both public and implementation-private fences. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitted_commands[0] = context->command;
	submitted_commands[1] = VK_NULL_HANDLE;
	submit.commandBufferCount = 2U;
	submit.pCommandBuffers = submitted_commands;
	mode = "null";
	if (round == 1U)
		mode = "explicit";

	/* POSIX process accounting measures this process's userspace and kernel CPU, excluding the peer. */
	error = load_cpu(&user_before, &system_before);
	if (error != 0)
		return error;

	/* Setup costs are outside the submit-through-completion measurement. */
	error = test_clock(&started);
	if (error != 0)
		return error;

	/* Each call is independently accepted; count alone does not prove simultaneous slot saturation. */
	for (index = 0U; index < FENCE_LOAD_SUBMITS; index++) {
		/* The middle round provides application-owned fences as a control. */
		fence = VK_NULL_HANDLE;
		if (round == 1U)
			fence = fences[index];

		/* This submission owns one distinct proof beside the unchanged heavy workload. */
		submitted_commands[1] = context->proof_commands[index];

		/* Short-lived pressure must be resolved inside the implementation without losing this work. */
		test_stage = "load vkQueueSubmit";
		status = vkQueueSubmit(context->queue, 1U, &submit, fence);
		test_vulkan_result = status;
		if (status != VK_SUCCESS) {
			printf("GPUFENCE LOAD FAILED role=%s round=%u submit=%u result=%d\n", test_role, round, index, status);
			return 1;
		}
	}

	/* Queue-idle covers every private fence and the earlier memory writes in this round. */
	test_stage = "load vkQueueWaitIdle";
	status = vkQueueWaitIdle(context->queue);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* The end timestamp excludes diagnostic readback and native teardown. */
	error = test_clock(&ended);
	if (error != 0)
		return error;

	/* The accounting window excludes diagnostic readback and uses the kernel's sampled CPU counters. */
	error = load_cpu(&user_after, &system_after);
	if (error != 0)
		return error;

	/* A regressing process counter cannot support a valid per-round CPU measurement. */
	if (user_after < user_before || system_after < system_before)
		return 1;

	/* Every word is checked after the real GPU work, not inferred from a successful API return. */
	error = load_verify(context);
	if (error != 0)
		return error;

	/* Every accepted submit must have executed its separate immutable proof recording. */
	error = load_proof_verify(context);
	if (error != 0)
		return error;

	/* Report the measured interval and the exact workload without claiming a performance multiplier. */
	printf("GPUFENCE LOAD role=%s round=%u mode=%s submits=%u bytes=%u repeats=%u elapsed_ms=%llu verified_bytes=%u user_cpu_us=%llu system_cpu_us=%llu verified_submits=%u\n", test_role, round, mode, FENCE_LOAD_SUBMITS, FENCE_LOAD_BYTES, FENCE_LOAD_REPEATS, (unsigned long long)((ended - started) / UINT64_C(1000000)), FENCE_LOAD_BYTES, (unsigned long long)(user_after - user_before), (unsigned long long)(system_after - system_before), FENCE_LOAD_SUBMITS);
	fflush(stdout);

	/* Succeeded: a full public-API submission window completed with verified GPU memory contents. */
	return 0;
}

/* Reads ordinary POSIX CPU accounting for one independent benchmark process. */
static int
load_cpu(
	uint64_t *user,
	uint64_t *system)
{
	struct rusage usage;
	int error;

	/* The process counters include its own threads, while excluding the independent peer and host renderer. */
	error = getrusage(RUSAGE_SELF, &usage);
	if (error != 0)
		return 1;

	/* Preserve microsecond units while leaving tick granularity visible in the recorded observations. */
	*user = (uint64_t)usage.ru_utime.tv_sec * UINT64_C(1000000) + (uint64_t)usage.ru_utime.tv_usec;
	*system = (uint64_t)usage.ru_stime.tv_sec * UINT64_C(1000000) + (uint64_t)usage.ru_stime.tv_usec;

	/* Succeeded: the caller can subtract two snapshots without guessing CPU time from wall time. */
	return 0;
}

/* Replaces the previous result after completion and before the next native submission. */
static int
load_clear(
	struct fence_test_context *context)
{
	void *mapping;
	VkResult status;

	/* Queue completion from the preceding round excludes any concurrent GPU access here. */
	mapping = NULL;
	test_stage = "load clear vkMapMemory";
	status = vkMapMemory(context->device, context->memory, 0U, FENCE_LOAD_BYTES, 0U, &mapping);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Coherent host writes precede submission and cannot already satisfy the GPU result oracle. */
	memset(mapping, 0, FENCE_LOAD_BYTES);
	vkUnmapMemory(context->device, context->memory);

	/* Succeeded: only newly executed transfer work can reproduce the expected result. */
	return 0;
}

/* Checks every word in the coherent allocation after the queue finishes writing it. */
static int
load_verify(
	struct fence_test_context *context)
{
	const uint32_t *words;
	void *mapping;
	VkResult status;
	unsigned index;
	int error;

	/* Mapping is diagnostic verification and is not part of the display or sharing path. */
	mapping = NULL;
	test_stage = "load vkMapMemory";
	status = vkMapMemory(context->device, context->memory, 0U, FENCE_LOAD_BYTES, 0U, &mapping);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* The selected host-coherent memory type and recorded barrier make every written word visible. */
	words = mapping;
	error = 0;
	for (index = 0U; index < FENCE_LOAD_BYTES / sizeof(*words); index++) {
		/* A successful fence is insufficient if its actual GPU output is wrong. */
		if (words[index] != 0x3197a5e2U) {
			printf("GPUFENCE LOAD FAILED role=%s word=%u actual=%x\n", test_role, index, words[index]);
			error = 1;
			break;
		}
	}

	/* No retained diagnostic mapping extends the next round or final object lifetime. */
	vkUnmapMemory(context->device, context->memory);
	if (error != 0)
		return error;

	/* Succeeded: the whole allocation contains the recorded transfer pattern. */
	return 0;
}

/* Creates one native logical device with explicit standard external-fence dependencies. */
static int
context_create(
	struct fence_test_context *context)
{
	const char *instance_extensions[2];
	const char *device_extensions[2];
	VkApplicationInfo app;
	VkInstanceCreateInfo instance;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	VkPhysicalDevice *physical;
	VkQueueFamilyProperties *families;
	VkPhysicalDeviceExternalFenceInfo query;
	VkExternalFenceProperties properties;
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult status;

	/* Instance and device extension dependencies follow Vulkan 1.0's promoted-feature rules. */
	memset(context, 0, sizeof(*context));
	instance_extensions[0] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
	instance_extensions[1] = VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME;
	device_extensions[0] = VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME;
	device_extensions[1] = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME;
	memset(&app, 0, sizeof(app));
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "gpu-fence";
	app.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &app;
	instance.enabledExtensionCount = 2;
	instance.ppEnabledExtensionNames = instance_extensions;
	test_stage = "vkCreateInstance";
	status = vkCreateInstance(&instance, NULL, &context->instance);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		check_result(status, VK_SUCCESS, "create instance");
		test_failure(__LINE__);
		return 1;
	}

	/* Select the first actual enumerated device, matching the single-GPU acceptance VM. */
	count = 0;
	test_stage = "vkEnumeratePhysicalDevices";
	status = vkEnumeratePhysicalDevices(context->instance, &count, NULL);
	test_vulkan_result = status;
	fprintf(stderr, "GPUFENCE CONTEXT role=%s physical_count=%u result=%d\n", test_role, count, status);
	if (status != VK_SUCCESS || count == 0) {
		test_failure(__LINE__);
		return 1;
	}

	physical = calloc(count, sizeof(*physical));
	if (physical == NULL) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkEnumeratePhysicalDevices";
	status = vkEnumeratePhysicalDevices(context->instance, &count, physical);
	test_vulkan_result = status;
	context->physical = physical[0];
	free(physical);
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Reject unsupported features rather than treating an absent backend as a passing test. */
	memset(&query, 0, sizeof(query));
	query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO;
	query.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&properties, 0, sizeof(properties));
	properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
	vkGetPhysicalDeviceExternalFencePropertiesKHR(context->physical, &query, &properties);
	test_stage = "external-fence capability";
	fprintf(stderr, "GPUFENCE CAPABILITY role=%s features=%u compatible=%u\n", test_role, properties.externalFenceFeatures, properties.compatibleHandleTypes);
	if ((properties.externalFenceFeatures & (VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) != (VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) {
		test_failure(__LINE__);
		return 1;
	}

	/* Find a transfer-capable graphics queue for a real GPU memory fill. */
	count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &count, NULL);
	fprintf(stderr, "GPUFENCE QUEUES role=%s count=%u\n", test_role, count);
	families = calloc(count, sizeof(*families));
	if (families == NULL) {
		test_failure(__LINE__);
		return 1;
	}

	vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &count, families);
	context->family = UINT32_MAX;
	for (index = 0; index < count; index++) {
		if (families[index].queueCount != 0 && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			context->family = index;
			break;
		}
	}

	free(families);
	if (context->family == UINT32_MAX) {
		test_failure(__LINE__);
		return 1;
	}

	/* The selected queue owns all work signaled by this process's exportable fence. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = context->family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1;
	device.pQueueCreateInfos = &queue;
	device.enabledExtensionCount = 2;
	device.ppEnabledExtensionNames = device_extensions;
	test_stage = "vkCreateDevice";
	status = vkCreateDevice(context->physical, &device, NULL, &context->device);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		check_result(status, VK_SUCCESS, "create device");
		test_failure(__LINE__);
		return 1;
	}

	vkGetDeviceQueue(context->device, context->family, 0, &context->queue);

	/* Succeeded: this process owns an independent standard Vulkan renderer context. */
	return 0;
}

/* Releases only resources whose real GPU work has completed in the normal scenario. */
static void
context_finish(
	struct fence_test_context *context)
{
	/* Fence destruction joins implementation completion work before native device close. */
	if (context->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(context->device);
		vkDestroyFence(context->device, context->shared, NULL);
		vkDestroyFence(context->device, context->private_fence, NULL);
		vkDestroyCommandPool(context->device, context->pool, NULL);
		vkDestroyEvent(context->device, context->event, NULL);
		vkDestroyBuffer(context->device, context->buffer, NULL);
		vkFreeMemory(context->device, context->memory, NULL);
		vkDestroyBuffer(context->device, context->proof_buffer, NULL);
		vkFreeMemory(context->device, context->proof_memory, NULL);
		vkDestroyDevice(context->device, NULL);
	}

	if (context->instance != VK_NULL_HANDLE)
		vkDestroyInstance(context->instance, NULL);

	/* Succeeded: ordinary objects no longer keep either process's GPU session alive. */
	return;
}

/* Constructs an ordinary or explicitly exportable fence with a known initial payload. */
static int
create_fence(
	struct fence_test_context *context,
	VkBool32 signaled,
	VkBool32 exportable,
	VkFence *fence)
{
	VkExportFenceCreateInfo export;
	VkFenceCreateInfo create;
	VkResult status;

	/* Exportability is selected at creation as required by vkGetFenceFdKHR. */
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
	export.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (exportable)
		create.pNext = &export;

	if (signaled)
		create.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	test_stage = "vkCreateFence";
	status = vkCreateFence(context->device, &create, NULL, fence);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		check_result(status, VK_SUCCESS, "create fence");
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: this object owns the requested initial standard fence payload. */
	return 0;
}

/* Records a real GPU fill, optionally preceded by an intentionally unsignaled host event. */
static int
create_work(
	struct fence_test_context *context,
	int blocked)
{
	int error;

	/* Keeps the original sharing test's small recording and explicit fault injection. */
	error = create_work_sized(context, blocked, 4096U, 1U, 0);
	if (error != 0)
		return error;

	/* Succeeded: the original sharing test has one ordinary GPU fill. */
	return 0;
}

/* Records ordered fills and a host visibility barrier for finite concurrent submissions. */
static int
create_work_sized(
	struct fence_test_context *context,
	int blocked,
	VkDeviceSize bytes,
	unsigned repeats,
	int simultaneous)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocation;
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkCommandBufferBeginInfo begin;
	VkEventCreateInfo event;
	VkMemoryBarrier barrier;
	VkResult status;
	uint32_t type;
	uint32_t attributes;
	unsigned index;

	/* The data path writes actual device storage even though this test validates completion only. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = bytes;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	test_stage = "vkCreateBuffer";
	status = vkCreateBuffer(context->device, &buffer, NULL, &context->buffer);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Load verification requires the actual written allocation to be coherently CPU visible. */
	vkGetBufferMemoryRequirements(context->device, context->buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(context->physical, &properties);
	for (type = 0; type < 32; type++) {
		/* A memory type must be supported by this specific native buffer. */
		if ((requirements.memoryTypeBits & (1U << type)) == 0)
			continue;

		/* The original completion-only test needs no CPU mapping. */
		if (simultaneous == 0)
			break;

		/* Property array bounds are independent from the requirement bit mask. */
		if (type >= properties.memoryTypeCount)
			continue;

		/* Shared coherent memory makes the final GPU bytes directly observable by the test. */
		attributes = properties.memoryTypes[type].propertyFlags;
		if ((attributes & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
			continue;

		/* Avoids accepting stale cached CPU data without explicit invalidation. */
		if ((attributes & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
			continue;

		/* This type supports both the native buffer and the verification mapping. */
		break;
	}

	if (type == 32) {
		test_failure(__LINE__);
		return 1;
	}

	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;
	test_stage = "vkAllocateMemory";
	status = vkAllocateMemory(context->device, &allocation, NULL, &context->memory);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkBindBufferMemory";
	status = vkBindBufferMemory(context->device, context->buffer, context->memory, 0);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.queueFamilyIndex = context->family;
	test_stage = "vkCreateCommandPool";
	status = vkCreateCommandPool(context->device, &pool, NULL, &context->pool);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = context->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1;
	test_stage = "vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(context->device, &command, &context->command);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* The explicit exit scenario retains a pending GPU dependency until process termination. */
	if (blocked) {
		memset(&event, 0, sizeof(event));
		event.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
		test_stage = "vkCreateEvent";
		status = vkCreateEvent(context->device, &event, NULL, &context->event);
		test_vulkan_result = status;
		if (status != VK_SUCCESS) {
			test_failure(__LINE__);
			return 1;
		}
	}

	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	/* Independent queued uses retain one immutable recording until the final queue wait. */
	if (simultaneous != 0)
		begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

	/* Publishes recording failure before any work can be submitted. */
	test_stage = "vkBeginCommandBuffer";
	status = vkBeginCommandBuffer(context->command, &begin);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	if (blocked)
		vkCmdWaitEvents(context->command, 1, &context->event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);

	/* Orders repeated writes, including writes from earlier uses of this recording on the queue. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	for (index = 0U; index < repeats; index++) {
		/* The load's data hazards are explicit rather than relying on submission order alone. */
		if (simultaneous != 0) {
			vkCmdPipelineBarrier(context->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 1U, &barrier, 0U, NULL, 0U, NULL);
		}

		/* Every submitted command performs a real write that the CPU checks after completion. */
		vkCmdFillBuffer(context->command, context->buffer, 0U, bytes, 0x3197a5e2U);
	}

	/* Makes transfer writes available for the final host verification after fence completion. */
	if (simultaneous != 0) {
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(context->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 1U, &barrier, 0U, NULL, 0U, NULL);
	}

	/* Recording must finish successfully before the application can borrow it for submission. */
	test_stage = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(context->command);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: command submission must execute GPU work before signaling its exportable fence. */
	return 0;
}

/* Submits the retained GPU fill with the current shared payload's native completion proof. */
static int
submit_work(
	struct fence_test_context *context)
{
	VkSubmitInfo submit;
	VkResult status;

	/* The same recording can be submitted again after the preceding fence has completed. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &context->command;
	test_stage = "vkQueueSubmit";
	status = vkQueueSubmit(context->queue, 1, &submit, context->shared);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		check_result(status, VK_SUCCESS, "submit real GPU work");
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: the implementation owns a pending shared signal, not a pre-signaled dummy. */
	return 0;
}

/* Sends one process-ordering acknowledgment over the same ordinary Unix stream. */
static int
send_byte(
	int socket,
	char value)
{
	ssize_t bytes;

	/* A one-byte transfer remains explicit so EOF and short writes are failures. */
	bytes = write(socket, &value, 1);
	if (bytes != 1) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: the peer may begin its next independently checked operation. */
	return 0;
}

/* Receives the exact peer acknowledgment expected by this phase of the test. */
static int
receive_byte(
	int socket,
	char expected)
{
	char value;
	ssize_t bytes;

	/* EOF must not be mistaken for permission to advance the acceptance scenario. */
	bytes = read(socket, &value, 1);
	if (bytes != 1 || value != expected) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: the peer has completed the preceding operation. */
	return 0;
}

/* Exports and duplicates one standard fence fd before SCM_RIGHTS transfer. */
static int
send_fence(
	int socket,
	struct fence_test_context *context,
	VkFence fence)
{
	VkFenceGetFdInfoKHR info;
	VkResult status;
	struct msghdr message;
	struct iovec vector;
	struct cmsghdr *control;
	union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary;
	int exported;
	int duplicate;
	int flags;
	char marker;
	ssize_t bytes;

	/* OPAQUE export preserves its payload and returns a fresh close-on-exec descriptor. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
	info.fence = fence;
	info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	test_stage = "vkGetFenceFdKHR";
	status = vkGetFenceFdKHR(context->device, &info, &exported);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	flags = fcntl(exported, F_GETFD);
	if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
		test_failure(__LINE__);
		return 1;
	}

	duplicate = dup(exported);
	close(exported);
	if (duplicate < 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* SCM_RIGHTS transfers a reference independently of the sender's two descriptor numbers. */
	marker = 'F';
	memset(&message, 0, sizeof(message));
	memset(&ancillary, 0, sizeof(ancillary));
	vector.iov_base = &marker;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = ancillary.bytes;
	message.msg_controllen = sizeof(ancillary.bytes);
	control = CMSG_FIRSTHDR(&message);
	control->cmsg_level = SOL_SOCKET;
	control->cmsg_type = SCM_RIGHTS;
	control->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(control), &duplicate, sizeof(duplicate));
	bytes = sendmsg(socket, &message, 0);
	close(duplicate);
	if (bytes != 1) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: the receiver owns a reference after every sender export fd is closed. */
	return 0;
}

/* Imports the descriptor delivered to this independent process by the kernel's Unix socket. */
static int
receive_fence(
	int socket,
	struct fence_test_context *context,
	VkFence fence,
	VkFenceImportFlags flags)
{
	VkImportFenceFdInfoKHR info;
	VkResult status;
	struct msghdr message;
	struct iovec vector;
	struct cmsghdr *control;
	union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary;
	int descriptor;
	char marker;
	ssize_t bytes;

	/* Receive exactly one rights-bearing protocol byte with enough ancillary capacity. */
	memset(&message, 0, sizeof(message));
	memset(&ancillary, 0, sizeof(ancillary));
	vector.iov_base = &marker;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = ancillary.bytes;
	message.msg_controllen = sizeof(ancillary.bytes);
	bytes = recvmsg(socket, &message, 0);
	if (bytes != 1 || marker != 'F' || (message.msg_flags & MSG_CTRUNC) != 0) {
		test_failure(__LINE__);
		return 1;
	}

	control = CMSG_FIRSTHDR(&message);
	if (control == NULL || control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS || control->cmsg_len != CMSG_LEN(sizeof(int))) {
		test_failure(__LINE__);
		return 1;
	}

	memcpy(&descriptor, CMSG_DATA(control), sizeof(descriptor));
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR;
	info.fence = fence;
	info.flags = flags;
	info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	info.fd = descriptor;
	test_stage = "vkImportFenceFdKHR";
	status = vkImportFenceFdKHR(context->device, &info);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		close(descriptor);
		check_result(status, VK_SUCCESS, "import received fence");
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: Vulkan owns the received descriptor; the application must not close it. */
	return 0;
}

/* Verifies shared resets and two real submissions from the original exporting process. */
static int
parent_test(
	int socket)
{
	struct fence_test_context context;
	VkResult status;
	int error;
	unsigned pass;

	/* The original payload starts signaled so the receiver can prove initial-state sharing. */
	error = context_create(&context);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = create_fence(&context, VK_TRUE, VK_TRUE, &context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = create_work(&context, 0);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = send_fence(socket, &context, context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The second iteration catches stale native-signaled reuse after another alias resets. */
	for (pass = 0; pass < 2; pass++) {
		error = receive_byte(socket, 'R');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		test_stage = "vkGetFenceStatus";
		status = vkGetFenceStatus(context.device, context.shared);
		test_vulkan_result = status;
		error = check_result(status, VK_NOT_READY, "receiver reset visible to producer");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		error = submit_work(&context);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		test_stage = "vkWaitForFences";
		status = vkWaitForFences(context.device, 1, &context.shared, VK_TRUE, FENCE_TEST_WAIT);
		test_vulkan_result = status;
		error = check_result(status, VK_SUCCESS, "producer actual GPU completion");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		error = send_byte(socket, 'S');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* A second transfer temporarily masks another local fence without consuming this payload. */
	error = send_fence(socket, &context, context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = receive_byte(socket, 'T');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkGetFenceStatus";
	status = vkGetFenceStatus(context.device, context.shared);
	test_vulkan_result = status;
	error = check_result(status, VK_SUCCESS, "temporary reset leaves imported payload signaled");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The receiver's retained reference must remain usable after the original Vulkan fence dies. */
	context_finish(&context);
	error = send_byte(socket, 'D');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = receive_byte(socket, 'C');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: real GPU execution, alias resets and source destruction preserved shared semantics. */
	return 0;
}

/* Verifies permanent and temporary imports from an independently initialized Vulkan instance. */
static int
child_test(
	int socket)
{
	struct fence_test_context context;
	VkFence candidates[2];
	VkResult status;
	int error;
	unsigned pass;

	/* An exportable receiving fence also permits later reference re-export by standard APIs. */
	error = context_create(&context);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = create_fence(&context, VK_FALSE, VK_TRUE, &context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = create_fence(&context, VK_TRUE, VK_FALSE, &context.private_fence);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = receive_fence(socket, &context, context.shared, 0);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkGetFenceStatus";
	status = vkGetFenceStatus(context.device, context.shared);
	test_vulkan_result = status;
	error = check_result(status, VK_SUCCESS, "imported initial signaled state");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Each reset operates on the same permanent kernel payload owned by the parent. */
	for (pass = 0; pass < 2; pass++) {
		test_stage = "vkResetFences";
		status = vkResetFences(context.device, 1, &context.shared);
		test_vulkan_result = status;
		if (status != VK_SUCCESS) {
			test_failure(__LINE__);
			return 1;
		}

		test_stage = "vkWaitForFences";
		status = vkWaitForFences(context.device, 1, &context.shared, VK_TRUE, 0);
		test_vulkan_result = status;
		error = check_result(status, VK_TIMEOUT, "zero timeout observes pending shared payload");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		test_stage = "vkWaitForFences";
		status = vkWaitForFences(context.device, 1, &context.shared, VK_TRUE, 1000000);
		test_vulkan_result = status;
		error = check_result(status, VK_TIMEOUT, "finite timeout preserves pending shared payload");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		error = send_byte(socket, 'R');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		test_stage = "vkWaitForFences";
		status = vkWaitForFences(context.device, 1, &context.shared, VK_TRUE, FENCE_TEST_WAIT);
		test_vulkan_result = status;
		error = check_result(status, VK_SUCCESS, "independent receiver GPU completion");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		error = receive_byte(socket, 'S');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* Temporary reset restores and unsignals the original permanent fence, leaving the import alone. */
	error = receive_fence(socket, &context, context.private_fence, VK_FENCE_IMPORT_TEMPORARY_BIT);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkResetFences";
	status = vkResetFences(context.device, 1, &context.private_fence);
	test_vulkan_result = status;
	if (status != VK_SUCCESS) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkGetFenceStatus";
	status = vkGetFenceStatus(context.device, context.private_fence);
	test_vulkan_result = status;
	error = check_result(status, VK_NOT_READY, "temporary reset restored permanent unsignaled state");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* A later signaled shared candidate satisfies any-wait while the ordinary fence stays pending. */
	candidates[0] = context.private_fence;
	candidates[1] = context.shared;
	test_stage = "vkWaitForFences";
	status = vkWaitForFences(context.device, 2, candidates, VK_FALSE, FENCE_TEST_WAIT);
	test_vulkan_result = status;
	error = check_result(status, VK_SUCCESS, "mixed any condition");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = send_byte(socket, 'T');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = receive_byte(socket, 'D');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkGetFenceStatus";
	status = vkGetFenceStatus(context.device, context.shared);
	test_vulkan_result = status;
	error = check_result(status, VK_SUCCESS, "payload survives original fence destruction");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	context_finish(&context);
	error = send_byte(socket, 'C');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: no receiver fd number or Vulkan object identity depended on the sender process. */
	return 0;
}

/* Sends the shared payload, submits, and leaves with a finite, delayed, or hung job outstanding. */
static int
exit_producer(
	int socket,
	int stop,
	int delayed)
{
	struct fence_test_context context;
	pid_t producer;
	int gated;
	int error;

	/* Suspension and delayed completion use valid finite work; only the plain or hung exit retains an unsignaled event. */
	gated = 0;
	if (stop != 0 || delayed != 0)
		gated = 1;

	/* This isolated fault scenario tests owner-session loss rather than a successful GPU completion of its own. */
	error = context_create(&context);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = create_fence(&context, VK_FALSE, VK_TRUE, &context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Gated scenarios record finite work the host can complete; the others wait on an event nobody sets. */
	if (gated != 0) {
		error = create_work(&context, 0);
	} else {
		error = create_work(&context, 1);
	}

	/* No submission may precede successful recording of the selected scenario. */
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The independent receiver owns the exact payload before the host delay is armed. */
	error = send_fence(socket, &context, context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Only the parent can permit submission after the isolated host gate exists. */
	if (gated != 0) {
		error = receive_byte(socket, 'G');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* A successful native transaction is independently supervised before userspace stops or exits. */
	error = submit_work(&context);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The receiver must observe native acceptance before checking the host's held callback. */
	error = send_byte(socket, 'B');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Stop every userspace thread with its fd live while the host retains the accepted job notification. */
	if (stop != 0) {
		test_stage = "producer SIGSTOP";
		producer = getpid();
		error = kill(producer, SIGSTOP);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* The parent releases the producer only after it has armed its own observation. */
	error = receive_byte(socket, 'X');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: process exit now retires the descriptors; the kernel alone supervises the outstanding job. */
	return 0;
}

/* Observes the imported payload after its producer stops, exits with a finite job, or exits with a hung job. */
static int
exit_consumer(
	int socket,
	pid_t stopped_producer,
	int delayed,
	int hang)
{
	struct fence_test_context context;
	struct timespec interval;
	VkResult status;
	VkResult expected;
	const char *label;
	uint64_t started;
	uint64_t finished;
	uint64_t now;
	uint64_t elapsed;
	int gated;
	int saved_error;
	int process_status;
	pid_t waited;
	int error;

	/* The stop and delayed scenarios hand the host exactly one native success to hold. */
	gated = 0;
	if (stopped_producer > 0 || delayed != 0)
		gated = 1;

	/* The receiver's device and descriptor references remain alive through producer death. */
	error = context_create(&context);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	error = create_fence(&context, VK_FALSE, VK_FALSE, &context.shared);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The consumer's own finite work later proves that its device survived the producer's loss. */
	if (stopped_producer <= 0) {
		error = create_work_sized(&context, 0, FENCE_LOAD_BYTES, 1U, 1);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		error = create_fence(&context, VK_FALSE, VK_FALSE, &context.private_fence);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	error = receive_fence(socket, &context, context.shared, 0);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* Arm the host's one-shot native-success delay before allowing this sole producer to submit. */
	if (gated != 0) {
		error = delay_gate("GPUFENCE DELAY_READY", "armed\n");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		/* The child cannot consume the host gate until both independent devices are ready. */
		error = send_byte(socket, 'G');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* Native acceptance precedes the stopped-state observation and imported payload wait. */
	error = receive_byte(socket, 'B');
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* The host must identify this actual native-success callback before the producer is released. */
	if (gated != 0) {
		error = delay_gate("GPUFENCE DELAY_SUBMITTED", "running\n");
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* The imported generation remains pending while its authoritative callback is held or hung. */
	test_stage = "vkGetFenceStatus";
	status = vkGetFenceStatus(context.device, context.shared);
	test_vulkan_result = status;
	error = check_result(status, VK_NOT_READY, "accepted producer notification remains pending");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* A stopped producer must be observed in the stopped state before the fence wait begins. */
	if (stopped_producer > 0) {
		test_stage = "waitpid producer stopped";
		waited = waitpid(stopped_producer, &process_status, WUNTRACED);
		if (waited != stopped_producer ||
		    !WIFSTOPPED(process_status) ||
		    WSTOPSIG(process_status) != SIGSTOP) {
			test_failure(__LINE__);
			return 1;
		}

		/* This marker certifies that final close and a running userspace worker cannot signal the fd. */
		puts("GPUFENCE PRODUCER_STOPPED pending=1 fd_live=1");
		fflush(stdout);
	} else {
		/* Every exit scenario ends the producer process before observing the terminal state. */
		error = send_byte(socket, 'X');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* Process teardown and its outstanding transport work have a separate finite test budget. */
	test_stage = "clock_gettime";
	error = test_clock(&started);
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	test_stage = "vkWaitForFences";
	status = vkWaitForFences(context.device, 1, &context.shared, VK_TRUE, FENCE_TEST_EXIT_WAIT);
	test_vulkan_result = status;
	saved_error = errno;

	/* Record actual wait duration without interpreting a valid timeout as producer completion. */
	test_stage = "clock_gettime";
	error = test_clock(&finished);
	if (error != 0 || finished < started) {
		test_failure(__LINE__);
		return 1;
	}

	/* Each scenario publishes its own label so the host verifier cannot confuse them. */
	label = "EXIT";
	if (stopped_producer > 0)
		label = "STOP";
	if (delayed != 0)
		label = "EXIT_DELAYED";
	if (hang != 0)
		label = "EXIT_HANG";

	elapsed = (finished - started) / UINT64_C(1000000);
	printf(
		"GPUFENCE PRODUCER_%s_WAIT result=%d elapsed_ms=%llu budget_ms=120000\n",
		label,
		status,
		(unsigned long long)elapsed);
	fflush(stdout);
	errno = saved_error;
	test_stage = "vkWaitForFences";

	/*
	 * A committed job outlives its producer and publishes its real result: the
	 * delayed finite job succeeds, the hung job fails at the execution deadline,
	 * and the stopped producer's job fails at that same deadline. A plain exit
	 * with an event nobody sets ends however the host actually ends it, success
	 * or deadline loss, but never as an error fabricated at producer close.
	 */
	expected = VK_ERROR_DEVICE_LOST;
	if (delayed != 0)
		expected = VK_SUCCESS;
	if (stopped_producer <= 0 && delayed == 0 && hang == 0 && status == VK_SUCCESS)
		expected = VK_SUCCESS;
	error = check_result(status, expected, "producer lifetime outcome is the job's real result, never fabricated");
	if (error != 0) {
		test_failure(__LINE__);
		return 1;
	}

	/* A plain exit can only end through the host's own completion or the sixty-second deadline. */
	if (stopped_producer <= 0 && delayed == 0 && hang == 0) {
		if (elapsed < 1000U || elapsed > 75000U) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* The delayed success must arrive with the host's fifteen-second notification, not immediately at exit. */
	if (delayed != 0) {
		if (elapsed < 14000U || elapsed > 30000U) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* The hung job must fail by the isolated eight-second execution deadline, not by producer exit. */
	if (hang != 0) {
		if (elapsed < 7000U || elapsed > 13000U) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* A live stopped producer must be supervised by the common execution deadline, independently from U. */
	if (stopped_producer > 0) {
		/* The isolated eight-second policy must fail before the host releases its fifteen-second callback. */
		if (elapsed < 7000U || elapsed > 13000U) {
			test_failure(__LINE__);
			return 1;
		}

		/* No process exit or spontaneous continue may explain the observed fence terminal state. */
		waited = waitpid(stopped_producer, &process_status, WNOHANG | WCONTINUED);
		if (waited != 0) {
			test_failure(__LINE__);
			return 1;
		}

		/* Resume only after terminal observation so the child can leave its process-owned descriptors. */
		error = kill(stopped_producer, SIGCONT);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		/* The existing acknowledgment makes final process exit and waitpid part of acceptance. */
		error = send_byte(socket, 'X');
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}
	}

	/* The ten-second stop interval must also elapse before peer survival says anything about isolation. */
	if (hang != 0) {
		interval.tv_sec = 0;
		interval.tv_nsec = 100000000L;
		for (;;) {
			error = test_clock(&now);
			if (error != 0) {
				test_failure(__LINE__);
				return 1;
			}

			/* Twenty-two seconds covers the eight-second deadline plus the ten-second stop interval. */
			if (now - started >= UINT64_C(22000000000))
				break;

			error = nanosleep(&interval, NULL);
			if (error != 0) {
				test_failure(__LINE__);
				return 1;
			}
		}
	}

	/* The consumer's own device must still execute real work after the producer's loss. */
	if (stopped_producer <= 0) {
		error = consumer_work(&context);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		error = test_clock(&now);
		if (error != 0) {
			test_failure(__LINE__);
			return 1;
		}

		/* The marker names which property was proven: continuation or survival of isolation. */
		if (hang != 0) {
			printf("GPUFENCE ISOLATION_PEER_OK elapsed_ms=%llu verified_bytes=%u\n", (unsigned long long)((now - started) / UINT64_C(1000000)), FENCE_LOAD_BYTES);
		} else {
			printf("GPUFENCE PEER_CONTINUES elapsed_ms=%llu verified_bytes=%u\n", (unsigned long long)((now - started) / UINT64_C(1000000)), FENCE_LOAD_BYTES);
		}
		fflush(stdout);
	}

	/* This deliberate lost-context case ends by process exit; its VM is discarded after evidence. */
	return 0;
}

/* Executes and verifies one finite transfer on the consumer's own device with its private fence. */
static int
consumer_work(
	struct fence_test_context *context)
{
	VkSubmitInfo submit;
	VkResult status;
	int error;

	/* A previous pattern cannot satisfy this round's result check. */
	error = load_clear(context);
	if (error != 0)
		return error;

	/* The private fence belongs to this device alone and never to the producer's shared payload. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &context->command;
	test_stage = "consumer vkQueueSubmit";
	status = vkQueueSubmit(context->queue, 1, &submit, context->private_fence);
	test_vulkan_result = status;
	error = check_result(status, VK_SUCCESS, "independent consumer submit after producer loss");
	if (error != 0)
		return error;

	/* Completion is tied to the real native queue and its authoritative callback. */
	test_stage = "consumer vkWaitForFences";
	status = vkWaitForFences(context->device, 1U, &context->private_fence, VK_TRUE, FENCE_TEST_WAIT);
	test_vulkan_result = status;
	error = check_result(status, VK_SUCCESS, "independent consumer completion after producer loss");
	if (error != 0)
		return error;

	/* Verify the actual transfer result after the successful completion. */
	error = load_verify(context);
	if (error != 0)
		return error;

	/* Succeeded: the consumer's device executed and completed real work. */
	return 0;
}

/*
 * Measures one monotonic nanosecond sample without changing Vulkan wait semantics.
 */
static int
test_clock(
	uint64_t *nanoseconds)
{
	struct timespec stamp;
	uint64_t seconds;
	int error;

	/* Invalid clocks fail the test instead of inventing an elapsed interval. */
	error = clock_gettime(CLOCK_MONOTONIC, &stamp);
	if (error != 0)
		return 1;

	if (stamp.tv_sec < 0 || stamp.tv_nsec < 0 || stamp.tv_nsec >= 1000000000L)
		return 1;

	/* Preserve exact finite measurement even near the representable clock limit. */
	seconds = (uint64_t)stamp.tv_sec;
	if (seconds > (UINT64_MAX - (uint64_t)stamp.tv_nsec) / UINT64_C(1000000000))
		return 1;

	*nanoseconds = seconds * UINT64_C(1000000000) + (uint64_t)stamp.tv_nsec;

	/* Succeeded: the caller can compare this sample with the same monotonic clock. */
	return 0;
}

/* Reports the actual Vulkan result beside the independent expected outcome. */
static int
check_result(
	VkResult actual,
	VkResult expected,
	const char *operation)
{
	/* Negative results are never normalized into successful test completion. */
	if (actual != expected) {
		fprintf(stderr, "GPUFENCE FAIL operation=%s actual=%d expected=%d\n", operation, actual, expected);
		test_failure(__LINE__);
		return 1;
	}

	/* Succeeded: this public API result matches the scenario's independent expectation. */
	return 0;
}

/* Identifies every failed guard without changing the actual pass/fail decision. */
static void
test_failure(
	unsigned line)
{
	int saved_error;

	/* Retain errno before stdio performs any operation of its own. */
	saved_error = errno;
	fprintf(stderr, "GPUFENCE FAIL role=%s stage=%s line=%u vk=%d errno=%d\n", test_role, test_stage, line, test_vulkan_result, saved_error);
	fflush(stderr);

	/* Succeeded: the caller still returns its original unsuccessful status. */
	return;
}
