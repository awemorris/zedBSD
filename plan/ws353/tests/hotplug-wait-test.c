/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The guest side of the libvulkan hotplug-fence and unsubmitted-fence wait
 * test (qemu-hotplug-wait.sh).  One run makes one instance and device and
 * runs the steps in order, writing one result line a step to DIR/result.txt
 * and a marker file DIR/NAME.armed when the host must change a head:
 *
 *   hotplug-wait-test DIR
 *
 *   idle-h     a wait on a hotplug fence alone sleeps 30 s in one poll
 *   idle-f     an any-wait on two unsubmitted fences sleeps 30 s in one poll
 *   submit-f   it returns when another thread submits the second fence
 *   narrow-f   it sleeps through another fence's 200 submissions, then returns at its own
 *   race-f     300 such waits whose fence is submitted just as they start, each with a 5 s limit
 *   idle-h2, submit-h, narrow-h, race-h
 *              the same any-waits with a hotplug fence as the first fence
 *   limit      destroyed fences leave no registration: 16 live ones fill the node, the 17th is refused
 *   plug       a hotplug fence wakes when the host plugs head 1; a reset fence stays spent
 *   unplug     a new fence wakes when the host unplugs head 1
 *   lost       a context failure ends a wait on a hotplug fence alone and an any-wait on two
 *              unsubmitted fences with VK_ERROR_DEVICE_LOST
 *
 * The steps with a hotplug fence need a GPU driver whose topology number moves only at a real
 * change: a renderer request between the registration and the change must not signal the fence.
 *
 * How often a wait slept is counted by this program's own poll(): libvulkan
 * calls poll() through the dynamic linker, which looks in the program first.
 * Only blocking polls (timeout not zero) of the thread being measured count.
 */

#include <vulkan/vulkan.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* One second of a Vulkan timeout. */
#define TEST_SECOND_NS		UINT64_C(1000000000)

/* The descriptors looked at for the renderer's context. */
#define TEST_FD_MAX		256

/* The rounds of the narrow and race steps. */
#define TEST_NARROW_ROUNDS	200U
#define TEST_RACE_ROUNDS	300U

/* The registrations one display node holds for one open. */
#define TEST_NODE_LIMIT		16U

/* One instance, its device and queue, and the entry point of the hotplug fence. */
struct test_state {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	PFN_vkRegisterDeviceEventEXT register_event;
	const char *directory;
	FILE *result;
	int failed;
	/* The descriptors vkCreateInstance and vkCreateDevice opened on GPU nodes. */
	int context_fds[TEST_FD_MAX];
	int context_count;
};

/* One any-wait run on its own thread: the fences, the limit, and what it saw. */
struct test_wait {
	struct test_state *state;
	VkFence fences[2];
	uint32_t count;
	uint64_t timeout;
	VkResult result;
	uint64_t returned_ms;
	int done;
};

/*
 * The thread whose blocking polls are counted, and the count.  The
 * thread sets itself before it waits; relaxed atomics, read after a join.
 */
static pthread_t counted_thread;
static int counting;
static unsigned counted_polls;

static int test_setup(struct test_state *state);
static void test_say(struct test_state *state, const char *format, ...);
static void test_marker(struct test_state *state, const char *name);
static uint64_t test_now_ms(void);
static void test_sleep_ms(unsigned milliseconds);
static VkResult test_fence(struct test_state *state, VkFence *fence);
static VkResult test_hotplug(struct test_state *state, VkFence *fence);
static VkResult test_submit(struct test_state *state, VkFence fence);
static void *test_waiter(void *argument);
static int test_wait_start(struct test_wait *wait, pthread_t *thread);
static void test_open_fds(int *open_fds);
static void step_idle(struct test_state *state, const char *name, VkFence partner, int alone);
static void step_submit(struct test_state *state, const char *name, VkFence partner);
static void step_narrow(struct test_state *state, const char *name, VkFence partner);
static void step_race(struct test_state *state, const char *name, VkFence partner);
static void step_limit(struct test_state *state);
static void step_plug(struct test_state *state);
static void step_lost(struct test_state *state);

/* Counts a blocking poll of the measured thread, then polls. */
int
poll(
	struct pollfd *fds,
	nfds_t count,
	int timeout_ms)
{
	struct timespec timeout;
	int measured;
	int ready;

	/* Only a sleeping poll of the measured thread counts. */
	measured = __atomic_load_n(&counting, __ATOMIC_ACQUIRE);
	if (measured != 0 && timeout_ms != 0) {
		if (pthread_equal(pthread_self(), counted_thread))
			__atomic_add_fetch(&counted_polls, 1U, __ATOMIC_RELAXED);
	}

	/* The libc's ppoll does the waiting. */
	if (timeout_ms < 0) {
		ready = ppoll(fds, count, NULL, NULL);
	} else {
		timeout.tv_sec = timeout_ms / 1000;
		timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
		ready = ppoll(fds, count, &timeout, NULL);
	}

	/* Reports what ppoll reported. */
	return ready;
}

/* Runs every step and writes the summary line. */
int
main(
	int argc,
	char **argv)
{
	struct test_state state;
	char path[256];
	FILE *done;
	VkFence hotplug;
	VkFence partner;
	VkResult error;
	int status;

	/* The result directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: hotplug-wait-test DIR\n");
		return 2;
	}
	memset(&state, 0, sizeof(state));
	state.directory = argv[1];
	snprintf(path, sizeof(path), "%s/result.txt", state.directory);
	state.result = fopen(path, "w");
	if (state.result == NULL)
		return 2;
	setvbuf(state.result, NULL, _IOLBF, 0);

	/* The host starts the program and returns before the GPU's open is logged on the console. */
	test_sleep_ms(3000U);

	/* The instance, device and queue. */
	status = test_setup(&state);
	if (status != 0) {
		test_say(&state, "setup: FAIL %d", status);
		test_say(&state, "hotplug-wait-test: FAIL");
		return 1;
	}

	/* A hotplug fence alone: no renderer request is made while it waits. */
	error = test_hotplug(&state, &hotplug);
	if (error != VK_SUCCESS) {
		test_say(&state, "setup: FAIL register %d", (int)error);
		test_say(&state, "hotplug-wait-test: FAIL");
		return 1;
	}
	step_idle(&state, "idle-h", hotplug, 1);
	vkDestroyFence(state.device, hotplug, NULL);

	/* Two unsubmitted fences: the waits sleep on the wake alone. */
	error = test_fence(&state, &partner);
	if (error != VK_SUCCESS) {
		test_say(&state, "setup: FAIL fence %d", (int)error);
		test_say(&state, "hotplug-wait-test: FAIL");
		return 1;
	}
	step_idle(&state, "idle-f", partner, 0);
	step_submit(&state, "submit-f", partner);
	step_narrow(&state, "narrow-f", partner);
	step_race(&state, "race-f", partner);
	vkDestroyFence(state.device, partner, NULL);

	/* A hotplug fence with an unsubmitted one. */
	error = test_hotplug(&state, &hotplug);
	if (error == VK_SUCCESS) {
		step_idle(&state, "idle-h2", hotplug, 0);
		step_submit(&state, "submit-h", hotplug);
		step_narrow(&state, "narrow-h", hotplug);
		step_race(&state, "race-h", hotplug);
		vkDestroyFence(state.device, hotplug, NULL);
	} else {
		test_say(&state, "idle-h2: FAIL register %d", (int)error);
	}
	step_limit(&state);

	/* The head changes, then the destructive step last. */
	step_plug(&state);
	step_lost(&state);

	/* The summary; the lost device is not torn down. */
	if (state.failed == 0) {
		test_say(&state, "hotplug-wait-test: PASS");
	} else {
		test_say(&state, "hotplug-wait-test: FAIL");
	}
	snprintf(path, sizeof(path), "%s/done", state.directory);
	done = fopen(path, "w");
	if (done != NULL)
		fclose(done);
	_exit(state.failed);
}

/* Makes the instance, the device with the display control and its queue. */
static int
test_setup(
	struct test_state *state)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_create;
	VkDeviceCreateInfo device_create;
	VkDeviceQueueCreateInfo queue_create;
	int before[TEST_FD_MAX];
	int after[TEST_FD_MAX];
	const char *instance_names[3];
	const char *device_names[2];
	uint32_t count;
	float priority;
	int fd;
	VkResult error;

	/* The descriptors open before Vulkan, so the ones it opens can be told apart. */
	test_open_fds(before);

	/* The direct-display instance with the surface counters display control needs. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "hotplug-wait-test";
	application.apiVersion = VK_API_VERSION_1_0;
	instance_names[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	instance_names[1] = VK_KHR_DISPLAY_EXTENSION_NAME;
	instance_names[2] = VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME;
	memset(&instance_create, 0, sizeof(instance_create));
	instance_create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_create.pApplicationInfo = &application;
	instance_create.enabledExtensionCount = 3U;
	instance_create.ppEnabledExtensionNames = instance_names;
	error = vkCreateInstance(&instance_create, NULL, &state->instance);
	if (error != VK_SUCCESS) {
		test_say(state, "setup: instance error=%d", (int)error);
		return 10;
	}

	/* The first physical device. */
	count = 1U;
	error = vkEnumeratePhysicalDevices(state->instance, &count, &state->physical);
	if ((error != VK_SUCCESS && error != VK_INCOMPLETE) || count == 0U) {
		test_say(state, "setup: physical devices error=%d count=%u", (int)error, count);
		return 11;
	}

	/* Family 0 has a queue that takes empty submissions; display control needs the swapchain. */
	priority = 1.0f;
	memset(&queue_create, 0, sizeof(queue_create));
	queue_create.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_create.queueFamilyIndex = 0U;
	queue_create.queueCount = 1U;
	queue_create.pQueuePriorities = &priority;
	device_names[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	device_names[1] = VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME;
	memset(&device_create, 0, sizeof(device_create));
	device_create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_create.queueCreateInfoCount = 1U;
	device_create.pQueueCreateInfos = &queue_create;
	device_create.enabledExtensionCount = 2U;
	device_create.ppEnabledExtensionNames = device_names;
	error = vkCreateDevice(state->physical, &device_create, NULL, &state->device);
	if (error != VK_SUCCESS) {
		test_say(state, "setup: device error=%d", (int)error);
		return 12;
	}
	vkGetDeviceQueue(state->device, 0U, 0U, &state->queue);

	/* The hotplug fence's entry point. */
	state->register_event = (PFN_vkRegisterDeviceEventEXT)vkGetDeviceProcAddr(state->device, "vkRegisterDeviceEventEXT");
	if (state->register_event == NULL)
		return 13;

	/* The renderer's context: the character devices Vulkan opened (the display nodes open later). */
	test_open_fds(after);
	state->context_count = 0;
	for (fd = 3; fd < TEST_FD_MAX; fd++) {
		if (after[fd] == 1 && before[fd] == 0)
			state->context_fds[state->context_count++] = fd;
	}
	test_say(state, "setup: context-fds=%d", state->context_count);

	/* Succeeded: the device is ready. */
	return 0;
}

/* Writes one result line, and keeps a failure. */
static void
test_say(
	struct test_state *state,
	const char *format,
	...)
{
	va_list arguments;
	char line[512];

	/* Formats the line. */
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* A FAIL anywhere fails the run. */
	if (strstr(line, ": FAIL") != NULL)
		state->failed = 1;

	/* The line goes to the result file at once. */
	fprintf(state->result, "%s\n", line);
	fflush(state->result);
}

/* Makes the marker file the host waits for before it changes a head. */
static void
test_marker(
	struct test_state *state,
	const char *name)
{
	char path[256];
	FILE *file;

	/* An empty file named after the step. */
	snprintf(path, sizeof(path), "%s/%s.armed", state->directory, name);
	file = fopen(path, "w");
	if (file != NULL)
		fclose(file);
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
test_now_ms(
	void)
{
	struct timespec now;

	/* The monotonic clock, as milliseconds. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Sleeps for some milliseconds. */
static void
test_sleep_ms(
	unsigned milliseconds)
{
	struct timespec delay;

	/* One nanosleep; an interruption only shortens it. */
	delay.tv_sec = milliseconds / 1000U;
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	nanosleep(&delay, NULL);
}

/* Makes an unsignaled ordinary fence. */
static VkResult
test_fence(
	struct test_state *state,
	VkFence *fence)
{
	VkFenceCreateInfo create;

	/* No flags: unsignaled, never submitted. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	return vkCreateFence(state->device, &create, NULL, fence);
}

/* Registers a hotplug fence. */
static VkResult
test_hotplug(
	struct test_state *state,
	VkFence *fence)
{
	VkDeviceEventInfoEXT event;

	/* The display hotplug event of the device. */
	memset(&event, 0, sizeof(event));
	event.sType = VK_STRUCTURE_TYPE_DEVICE_EVENT_INFO_EXT;
	event.deviceEvent = VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT;
	return state->register_event(state->device, &event, NULL, fence);
}

/* Signals a fence by an empty submission. */
static VkResult
test_submit(
	struct test_state *state,
	VkFence fence)
{
	/* No work: the fence is signaled once the queue reaches it. */
	return vkQueueSubmit(state->queue, 0U, NULL, fence);
}

/* Runs one measured any-wait on its own thread. */
static void *
test_waiter(
	void *argument)
{
	struct test_wait *wait;

	/* This thread's blocking polls are the ones counted. */
	wait = argument;
	counted_thread = pthread_self();
	__atomic_store_n(&counted_polls, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&counting, 1, __ATOMIC_RELEASE);

	/* Waits for either fence. */
	wait->result = vkWaitForFences(wait->state->device, wait->count, wait->fences, VK_FALSE, wait->timeout);
	wait->returned_ms = test_now_ms();
	__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&wait->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

/* Starts a waiter and returns once it is about to wait (its counting is on). */
static int
test_wait_start(
	struct test_wait *wait,
	pthread_t *thread)
{
	int error;

	/* The thread and its first moment. */
	__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
	wait->done = 0;
	error = pthread_create(thread, NULL, test_waiter, wait);
	if (error != 0)
		return error;

	/* The waiter turns counting on just before it calls vkWaitForFences. */
	while (__atomic_load_n(&counting, __ATOMIC_ACQUIRE) == 0 && __atomic_load_n(&wait->done, __ATOMIC_ACQUIRE) == 0)
		sched_yield();
	return 0;
}

/* Marks which descriptors below TEST_FD_MAX are open character devices. */
static void
test_open_fds(
	int *open_fds)
{
	struct stat status;
	int fd;
	int error;

	/* fstat answers for an open descriptor only. */
	for (fd = 0; fd < TEST_FD_MAX; fd++) {
		open_fds[fd] = 0;
		error = fstat(fd, &status);
		if (error == 0 && S_ISCHR(status.st_mode))
			open_fds[fd] = 1;
	}
}

/* idle: the wait sleeps once for 30 s and times out. */
static void
step_idle(
	struct test_state *state,
	const char *name,
	VkFence partner,
	int alone)
{
	struct test_wait wait;
	pthread_t thread;
	VkFence fence;
	uint64_t started;
	unsigned polls;

	/* The partner alone, or with an unsubmitted fence, 30 s. */
	fence = VK_NULL_HANDLE;
	if (!alone && test_fence(state, &fence) != VK_SUCCESS) {
		test_say(state, "%s: FAIL fence", name);
		return;
	}
	memset(&wait, 0, sizeof(wait));
	wait.state = state;
	wait.fences[0] = partner;
	wait.fences[1] = fence;
	wait.count = 2U;
	if (alone)
		wait.count = 1U;
	wait.timeout = 30U * TEST_SECOND_NS;
	started = test_now_ms();
	test_wait_start(&wait, &thread);
	pthread_join(thread, NULL);
	polls = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);

	/* One blocking poll for the whole wait, and a timeout at its end. */
	if (wait.result == VK_TIMEOUT && polls == 1U && wait.returned_ms - started >= 29000U) {
		test_say(state, "%s: PASS result=%d polls=%u ms=%llu", name, (int)wait.result, polls,
		    (unsigned long long)(wait.returned_ms - started));
	} else {
		test_say(state, "%s: FAIL result=%d polls=%u ms=%llu", name, (int)wait.result, polls,
		    (unsigned long long)(wait.returned_ms - started));
	}
	vkDestroyFence(state->device, fence, NULL);
}

/* submit: another thread's empty submission of the unsubmitted fence ends the wait. */
static void
step_submit(
	struct test_state *state,
	const char *name,
	VkFence partner)
{
	struct test_wait wait;
	pthread_t thread;
	VkFence fence;
	VkResult submitted;
	VkResult fence_status;
	VkResult hotplug_status;
	uint64_t at;
	unsigned polls;

	/* The waiter sleeps on both fences. */
	if (test_fence(state, &fence) != VK_SUCCESS) {
		test_say(state, "%s: FAIL fence", name);
		return;
	}
	memset(&wait, 0, sizeof(wait));
	wait.state = state;
	wait.fences[0] = partner;
	wait.fences[1] = fence;
	wait.count = 2U;
	wait.timeout = UINT64_MAX;
	test_wait_start(&wait, &thread);

	/* The submission, half a second later. */
	test_sleep_ms(500U);
	at = test_now_ms();
	submitted = test_submit(state, fence);
	pthread_join(thread, NULL);
	polls = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);
	fence_status = vkGetFenceStatus(state->device, fence);
	hotplug_status = vkGetFenceStatus(state->device, partner);

	/* The wait returned soon after the submission, with the submitted fence signaled. */
	if (submitted == VK_SUCCESS &&
	    wait.result == VK_SUCCESS &&
	    fence_status == VK_SUCCESS &&
	    hotplug_status == VK_NOT_READY &&
	    wait.returned_ms - at < 200U) {
		test_say(state, "%s: PASS ms=%llu polls=%u", name, (unsigned long long)(wait.returned_ms - at), polls);
	} else {
		test_say(state, "%s: FAIL submit=%d result=%d fence=%d hotplug=%d ms=%llu polls=%u", name, (int)submitted,
		    (int)wait.result, (int)fence_status, (int)hotplug_status, (unsigned long long)(wait.returned_ms - at), polls);
	}
	vkDestroyFence(state->device, fence, NULL);
}

/* narrow: another fence's submissions do not wake the wait; its own does. */
static void
step_narrow(
	struct test_state *state,
	const char *name,
	VkFence partner)
{
	struct test_wait wait;
	pthread_t thread;
	VkFence fence;
	VkFence other;
	VkResult error;
	unsigned round;
	unsigned during;
	unsigned polls;

	/* The waited fence and the other one. */
	if (test_fence(state, &fence) != VK_SUCCESS || test_fence(state, &other) != VK_SUCCESS) {
		test_say(state, "%s: FAIL fence", name);
		return;
	}
	memset(&wait, 0, sizeof(wait));
	wait.state = state;
	wait.fences[0] = partner;
	wait.fences[1] = fence;
	wait.count = 2U;
	wait.timeout = UINT64_MAX;
	test_wait_start(&wait, &thread);
	test_sleep_ms(200U);

	/* The other fence submitted, waited for and reset again and again. */
	error = VK_SUCCESS;
	for (round = 0U; round < TEST_NARROW_ROUNDS && error == VK_SUCCESS; round++) {
		error = test_submit(state, other);
		if (error == VK_SUCCESS)
			error = vkWaitForFences(state->device, 1U, &other, VK_TRUE, 5U * TEST_SECOND_NS);
		if (error == VK_SUCCESS)
			error = vkResetFences(state->device, 1U, &other);
	}
	test_sleep_ms(200U);
	during = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);

	/* Its own fence ends it. */
	if (error == VK_SUCCESS)
		error = test_submit(state, fence);
	pthread_join(thread, NULL);
	polls = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);

	/* The wait slept once through all of the other fence's rounds. */
	if (error == VK_SUCCESS && wait.result == VK_SUCCESS && during <= 1U) {
		test_say(state, "%s: PASS rounds=%u polls-during=%u polls=%u", name, round, during, polls);
	} else {
		test_say(state, "%s: FAIL error=%d result=%d rounds=%u polls-during=%u polls=%u", name, (int)error, (int)wait.result,
		    round, during, polls);
	}
	vkDestroyFence(state->device, fence, NULL);
	vkDestroyFence(state->device, other, NULL);
}

/* race: the fence is submitted just as the wait starts, many times; no wait misses it. */
static void
step_race(
	struct test_state *state,
	const char *name,
	VkFence partner)
{
	struct test_wait wait;
	pthread_t thread;
	VkFence fence;
	VkFence other;
	VkResult error;
	unsigned round;
	unsigned spin;
	unsigned extra;
	unsigned missed;
	unsigned failed;
	volatile unsigned sink;

	/* The raced fence and another one submitted around it. */
	if (test_fence(state, &fence) != VK_SUCCESS || test_fence(state, &other) != VK_SUCCESS) {
		test_say(state, "%s: FAIL fence", name);
		return;
	}
	missed = 0U;
	failed = 0U;
	srand(353U);
	for (round = 0U; round < TEST_RACE_ROUNDS; round++) {
		/* A fresh wait on the unsignaled fence. */
		memset(&wait, 0, sizeof(wait));
		wait.state = state;
		wait.fences[0] = partner;
		wait.fences[1] = fence;
		wait.count = 2U;
	wait.timeout = 5U * TEST_SECOND_NS;
		test_wait_start(&wait, &thread);

		/* A short random spin, other submissions, then the fence. */
		spin = (unsigned)rand() % 20000U;
		for (sink = 0U; sink < spin; sink++)
			;
		error = VK_SUCCESS;
		for (extra = (unsigned)rand() % 3U; extra > 0U && error == VK_SUCCESS; extra--) {
			error = test_submit(state, other);
			if (error == VK_SUCCESS)
				error = vkWaitForFences(state->device, 1U, &other, VK_TRUE, 5U * TEST_SECOND_NS);
			if (error == VK_SUCCESS)
				error = vkResetFences(state->device, 1U, &other);
		}
		if (error == VK_SUCCESS)
			error = test_submit(state, fence);
		pthread_join(thread, NULL);

		/* A wait that timed out missed the submission. */
		if (wait.result == VK_TIMEOUT)
			missed++;
		else if (wait.result != VK_SUCCESS || error != VK_SUCCESS)
			failed++;

		/* The next round starts from an unsignaled fence. */
		error = vkResetFences(state->device, 1U, &fence);
		if (error != VK_SUCCESS)
			failed++;
	}

	/* Every wait returned with its fence. */
	if (missed == 0U && failed == 0U) {
		test_say(state, "%s: PASS rounds=%u", name, TEST_RACE_ROUNDS);
	} else {
		test_say(state, "%s: FAIL rounds=%u missed=%u failed=%u", name, TEST_RACE_ROUNDS, missed, failed);
	}
	vkDestroyFence(state->device, fence, NULL);
	vkDestroyFence(state->device, other, NULL);
}

/* limit: destroyed fences leave no registration; 16 live ones fill the node, the 17th is refused. */
static void
step_limit(
	struct test_state *state)
{
	VkFence fences[TEST_NODE_LIMIT];
	int live[TEST_NODE_LIMIT];
	VkFence fence;
	VkResult error;
	VkResult refused;
	VkResult again;
	unsigned index;
	unsigned made;

	/* Forty registrations destroyed at once: a leak would fill the node after sixteen. */
	error = VK_SUCCESS;
	for (index = 0U; index < 40U && error == VK_SUCCESS; index++) {
		error = test_hotplug(state, &fence);
		if (error == VK_SUCCESS)
			vkDestroyFence(state->device, fence, NULL);
	}
	if (error != VK_SUCCESS) {
		test_say(state, "limit: FAIL churn index=%u error=%d", index, (int)error);
		return;
	}

	/* Sixteen live ones, then one too many. */
	made = 0U;
	for (index = 0U; index < TEST_NODE_LIMIT; index++) {
		live[index] = 0;
		error = test_hotplug(state, &fences[index]);
		if (error == VK_SUCCESS) {
			live[index] = 1;
			made++;
		}
	}
	refused = test_hotplug(state, &fence);
	if (refused == VK_SUCCESS)
		vkDestroyFence(state->device, fence, NULL);

	/* One destroyed, one made again in its place. */
	again = VK_ERROR_UNKNOWN;
	if (live[0] != 0) {
		vkDestroyFence(state->device, fences[0], NULL);
		live[0] = 0;
		again = test_hotplug(state, &fences[0]);
		if (again == VK_SUCCESS)
			live[0] = 1;
	}

	/* The node held sixteen, refused the seventeenth, and took one after a destruction. */
	if (made == TEST_NODE_LIMIT && refused == VK_ERROR_OUT_OF_HOST_MEMORY && again == VK_SUCCESS) {
		test_say(state, "limit: PASS");
	} else {
		test_say(state, "limit: FAIL made=%u refused=%d again=%d", made, (int)refused, (int)again);
	}

	/* Every live registration goes. */
	for (index = 0U; index < TEST_NODE_LIMIT; index++) {
		if (live[index] != 0)
			vkDestroyFence(state->device, fences[index], NULL);
	}
}

/* plug and unplug: the host changes head 1 after each marker; the fences wake; a reset one stays spent. */
static void
step_plug(
	struct test_state *state)
{
	VkFence plugged;
	VkFence unplugged;
	VkResult error;
	VkResult after_reset;
	VkResult after_unplug;
	uint64_t armed;
	uint64_t woke;
	unsigned polls;

	/* Waits for the plug on this thread, counting its sleeps. */
	error = test_hotplug(state, &plugged);
	if (error != VK_SUCCESS) {
		test_say(state, "plug: FAIL register %d", (int)error);
		return;
	}
	counted_thread = pthread_self();
	__atomic_store_n(&counted_polls, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&counting, 1, __ATOMIC_RELEASE);
	test_marker(state, "plug");
	armed = test_now_ms();
	error = vkWaitForFences(state->device, 1U, &plugged, VK_TRUE, 30U * TEST_SECOND_NS);
	woke = test_now_ms();
	__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
	polls = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);

	/* A signaled fence that is reset is spent: no later change signals it. */
	vkResetFences(state->device, 1U, &plugged);
	after_reset = vkGetFenceStatus(state->device, plugged);
	if (error == VK_SUCCESS && after_reset == VK_NOT_READY) {
		test_say(state, "plug: PASS ms=%llu polls=%u", (unsigned long long)(woke - armed), polls);
	} else {
		test_say(state, "plug: FAIL result=%d after-reset=%d ms=%llu polls=%u", (int)error, (int)after_reset,
		    (unsigned long long)(woke - armed), polls);
	}

	/* A new fence for the unplug. */
	error = test_hotplug(state, &unplugged);
	if (error != VK_SUCCESS) {
		test_say(state, "unplug: FAIL register %d", (int)error);
		vkDestroyFence(state->device, plugged, NULL);
		return;
	}
	test_marker(state, "unplug");
	armed = test_now_ms();
	error = vkWaitForFences(state->device, 1U, &unplugged, VK_TRUE, 30U * TEST_SECOND_NS);
	woke = test_now_ms();
	after_unplug = vkGetFenceStatus(state->device, plugged);
	if (error == VK_SUCCESS && after_unplug == VK_NOT_READY) {
		test_say(state, "unplug: PASS ms=%llu spent=%d", (unsigned long long)(woke - armed), (int)after_unplug);
	} else {
		test_say(state, "unplug: FAIL result=%d spent=%d ms=%llu", (int)error, (int)after_unplug,
		    (unsigned long long)(woke - armed));
	}
	vkDestroyFence(state->device, plugged, NULL);
	vkDestroyFence(state->device, unplugged, NULL);
}

/*
 * lost: the renderer's context fails under two waits, one on a hotplug
 * fence alone and an any-wait on two unsubmitted fences; both end with
 * VK_ERROR_DEVICE_LOST.  The hotplug fence is registered after the other
 * fences, and its wait makes no renderer request, so only the failure ends it.
 */
static void
step_lost(
	struct test_state *state)
{
	struct test_wait hotplug_wait;
	struct test_wait fence_wait;
	pthread_t hotplug_thread;
	pthread_t fence_thread;
	VkFence hotplug;
	VkFence first;
	VkFence second;
	VkFence other;
	VkResult submitted;
	unsigned tries;
	int null_fd;
	int index;
	int done;

	/* The fences, the hotplug one last. */
	if (state->context_count == 0) {
		test_say(state, "lost: FAIL no context descriptor");
		return;
	}
	if (test_fence(state, &first) != VK_SUCCESS ||
	    test_fence(state, &second) != VK_SUCCESS ||
	    test_fence(state, &other) != VK_SUCCESS ||
	    test_hotplug(state, &hotplug) != VK_SUCCESS) {
		test_say(state, "lost: FAIL fence");
		return;
	}

	/* The two waits. */
	memset(&hotplug_wait, 0, sizeof(hotplug_wait));
	hotplug_wait.state = state;
	hotplug_wait.fences[0] = hotplug;
	hotplug_wait.count = 1U;
	hotplug_wait.timeout = UINT64_MAX;
	memset(&fence_wait, 0, sizeof(fence_wait));
	fence_wait.state = state;
	fence_wait.fences[0] = first;
	fence_wait.fences[1] = second;
	fence_wait.count = 2U;
	fence_wait.timeout = UINT64_MAX;
	test_wait_start(&hotplug_wait, &hotplug_thread);
	test_wait_start(&fence_wait, &fence_thread);
	test_sleep_ms(500U);

	/* Neither wait may have ended before the failure. */
	if (__atomic_load_n(&hotplug_wait.done, __ATOMIC_ACQUIRE) != 0 ||
	    __atomic_load_n(&fence_wait.done, __ATOMIC_ACQUIRE) != 0) {
		test_say(state, "lost: FAIL a wait ended early hotplug=%d fences=%d", (int)hotplug_wait.result, (int)fence_wait.result);
		return;
	}

	/*
	 * The context's descriptor now names /dev/null: the next submission's
	 * request fails as a lost renderer's would, and the poll the waits
	 * sleep in sees nothing of it.
	 */
	null_fd = open("/dev/null", O_RDWR);
	for (index = 0; index < state->context_count; index++)
		dup2(null_fd, state->context_fds[index]);
	submitted = test_submit(state, other);

	/* Both waits end within 5 s. */
	done = 0;
	for (tries = 0U; tries < 50U && done == 0; tries++) {
		test_sleep_ms(100U);
		if (__atomic_load_n(&hotplug_wait.done, __ATOMIC_ACQUIRE) != 0 &&
		    __atomic_load_n(&fence_wait.done, __ATOMIC_ACQUIRE) != 0)
			done = 1;
	}
	if (done == 0) {
		test_say(state, "lost: FAIL a wait did not return submit=%d hotplug-done=%d fences-done=%d", (int)submitted,
		    hotplug_wait.done, fence_wait.done);
		return;
	}
	pthread_join(hotplug_thread, NULL);
	pthread_join(fence_thread, NULL);
	if (hotplug_wait.result == VK_ERROR_DEVICE_LOST && fence_wait.result == VK_ERROR_DEVICE_LOST) {
		test_say(state, "lost: PASS submit=%d", (int)submitted);
	} else {
		test_say(state, "lost: FAIL hotplug=%d fences=%d submit=%d", (int)hotplug_wait.result, (int)fence_wait.result,
		    (int)submitted);
	}
}
