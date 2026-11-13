/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The guest side of the libvulkan present-fence test (qemu-present-fence.sh):
 * VK_EXT_swapchain_maintenance1 and the surface extensions it depends on,
 * on direct-display swapchains.  One run makes one instance and device on
 * two displays (head 0 and head 1) and runs the steps in order, writing one
 * result line a step to DIR/result.txt and a marker file DIR/NAME.armed when
 * the host must change a head:
 *
 *   present-fence-test DIR
 *
 *   ext        the three extensions are offered, the device takes them, the feature is reported
 *   caps2      the extensible surface queries: FIFO is compatible with itself alone, no scaling,
 *              the same formats as the ordinary query, VK_INCOMPLETE for a short array
 *   create     a swapchain naming other present modes is refused; one naming its own mode,
 *              no scaling and a deferred allocation is made
 *   release    an acquired image released is acquired again; a second release is refused
 *   unenqueued a refused present (an image not acquired) leaves its fence unsignaled
 *   frames     100 presents, each with a fence another thread waits for with UINT64_MAX:
 *              every fence signals, in present order, at the presents' pace, which FIFO keeps
 *              to whole refresh periods
 *   pace       60 presents without fences: the presents' own pace, to compare with
 *   narrow     an acquisition blocked on head 1's swapchain sleeps in one poll while
 *              head 0's swapchain presents 60 frames
 *   destroy    the fences of presents still queued are signaled once the swapchain is destroyed
 *   unplug     while head 1's swapchain presents, the host unplugs head 1: the presents fail,
 *              and every fence of an enqueued present still signals
 *
 * How often the acquisition slept is counted by this program's own ppoll():
 * libvulkan calls ppoll() through the dynamic linker, which looks in the
 * program first.  Only blocking polls of the thread being measured count.
 */

#include <vulkan/vulkan.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uapi/syscall.h>

/* One second of a Vulkan timeout. */
#define TEST_SECOND_NS		UINT64_C(1000000000)

/* The presents of the frames, narrow and destroy steps. */
#define TEST_FRAMES		100U
#define TEST_NARROW_FRAMES	60U
#define TEST_DESTROY_FRAMES	3U

/* The most presents the unplug step makes before the head goes. */
#define TEST_UNPLUG_FRAMES	4000U

/* The most displays and swapchain images looked at. */
#define TEST_DISPLAYS		8U
#define TEST_IMAGES		8U

/* The libc's system call entry, which the ppoll below calls in place of the libc's ppoll. */
extern intptr_t __syscall6(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

/* One display's surface and swapchain, with its images and mode. */
struct test_output {
	VkDisplayKHR display;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkImage images[TEST_IMAGES];
	uint32_t image_count;
	VkExtent2D extent;
	VkSurfaceFormatKHR format;
	uint32_t minimum;
	uint32_t refresh_mhz;
	VkClearColorValue colour;
	int cleared;
};

/* The fences of a run of presents and the thread that waits for them, in order. */
struct test_waiter {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	VkDevice device;
	VkFence *fences;
	uint64_t *signaled_ms;
	uint32_t capacity;
	uint32_t submitted;
	uint32_t observed;
	uint32_t out_of_order;
	VkResult error;
	int finished;
	int exited;
};

/* The program's Vulkan objects and its result file. */
struct test_state {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
	VkCommandBuffer commands;
	VkFence done;
	VkDisplayKHR displays[TEST_DISPLAYS];
	uint32_t display_count;
	uint32_t plane_used;
	const char *directory;
	FILE *result;
	int failed;
};

/* One acquisition on its own thread, measured by its blocking polls. */
struct test_acquire {
	struct test_state *state;
	struct test_output *output;
	uint64_t timeout;
	VkFence fence;
	VkResult result;
	unsigned polls;
};

/*
 * The thread whose blocking polls are counted, and the count.  The thread
 * sets itself before it waits; relaxed atomics, read after a join.
 */
static pthread_t counted_thread;
static int counting;
static unsigned counted_polls;

static int test_setup(struct test_state *state);
static void test_say(struct test_state *state, const char *format, ...);
static void test_marker(struct test_state *state, const char *name);
static uint64_t test_now_ms(void);
static void test_sleep_ms(unsigned milliseconds);
static int test_has_extension(const VkExtensionProperties *properties, uint32_t count, const char *name);
static VkResult test_surface(struct test_state *state, uint32_t index, struct test_output *output);
static VkResult test_swapchain(struct test_state *state, struct test_output *output, const void *next, VkSwapchainCreateFlagsKHR flags);
static VkResult test_fence(struct test_state *state, VkFence *fence);
static VkResult test_acquire_image(struct test_state *state, struct test_output *output, uint64_t timeout, uint32_t *image);
static VkResult test_clear(struct test_state *state, struct test_output *output, uint32_t image);
static VkResult test_present(struct test_state *state, struct test_output *output, uint32_t image, VkFence fence, VkSemaphore wait);
static int test_waiter_init(struct test_waiter *waiter, VkDevice device, uint32_t capacity);
static void test_waiter_add(struct test_waiter *waiter, VkFence fence);
static void test_waiter_finish(struct test_waiter *waiter);
static int test_waiter_join(struct test_waiter *waiter, pthread_t thread, unsigned limit_ms);
static void *test_waiter_main(void *argument);
static void *test_acquire_main(void *argument);
static void step_ext(struct test_state *state);
static void step_caps2(struct test_state *state, struct test_output *output);
static void step_create(struct test_state *state, struct test_output *output);
static void step_release(struct test_state *state, struct test_output *output);
static void step_unenqueued(struct test_state *state, struct test_output *output);
static uint64_t test_median(uint64_t *values, uint32_t count);
static void step_frames(struct test_state *state, struct test_output *output, const char *name, uint32_t frames, uint32_t check_rate);
static void step_pace(struct test_state *state, struct test_output *output);
static void step_narrow(struct test_state *state, struct test_output *output, struct test_output *other);
static void step_destroy(struct test_state *state, struct test_output *output);
static void step_unplug(struct test_state *state, struct test_output *output);

/* Counts a blocking ppoll of the measured thread, then waits with the system call itself. */
int
ppoll(
	struct pollfd *fds,
	nfds_t count,
	const struct timespec *timeout,
	const sigset_t *mask)
{
	intptr_t ready;
	int measured;

	/* Only a sleeping poll of the measured thread counts. */
	measured = __atomic_load_n(&counting, __ATOMIC_ACQUIRE);
	if (measured != 0) {
		/* A zero timeout only looks. */
		if (timeout == NULL || timeout->tv_sec != 0 || timeout->tv_nsec != 0) {
			if (pthread_equal(pthread_self(), counted_thread))
				__atomic_add_fetch(&counted_polls, 1U, __ATOMIC_RELAXED);
		}
	}

	/* The kernel's ppoll, as the libc's (libvulkan passes no signal mask). */
	ready = __syscall6(KERN_SYS_ppoll, (uintptr_t)fds, (uintptr_t)count, (uintptr_t)timeout, (uintptr_t)mask, 0U, 0U);
	if (ready < 0 && ready >= -4095) {
		errno = (int)-ready;
		return -1;
	}

	/* Reports how many descriptors are ready. */
	return (int)ready;
}

/* Waits with ppoll above, so a poll of the measured thread is counted once. */
int
poll(
	struct pollfd *fds,
	nfds_t count,
	int timeout_ms)
{
	struct timespec timeout;
	int ready;

	/* A negative timeout waits without a limit. */
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
	struct test_output first;
	struct test_output second;
	char path[256];
	FILE *done;
	VkResult error;
	int status;

	/* The result directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: present-fence-test DIR\n");
		return 2;
	}
	memset(&state, 0, sizeof(state));
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	state.directory = argv[1];
	snprintf(path, sizeof(path), "%s/result.txt", state.directory);
	state.result = fopen(path, "w");
	if (state.result == NULL)
		return 2;
	setvbuf(state.result, NULL, _IOLBF, 0);

	/* The host starts the program and returns before the GPU's open is logged on the console. */
	test_sleep_ms(3000U);

	/* The instance, the device, its queue and its clear commands. */
	status = test_setup(&state);
	if (status != 0) {
		test_say(&state, "setup: FAIL %d", status);
		test_say(&state, "present-fence-test: FAIL");
		return 1;
	}
	step_ext(&state);

	/* Head 0 and head 1, each with a surface of its own. */
	if (state.display_count < 2U) {
		test_say(&state, "setup: FAIL displays=%u (the host gives head 1 a size first)", state.display_count);
		test_say(&state, "present-fence-test: FAIL");
		return 1;
	}
	error = test_surface(&state, 0U, &first);
	if (error == VK_SUCCESS)
		error = test_surface(&state, 1U, &second);
	if (error != VK_SUCCESS) {
		test_say(&state, "setup: FAIL surface %d", (int)error);
		test_say(&state, "present-fence-test: FAIL");
		return 1;
	}
	first.colour.float32[0] = 0.2f;
	first.colour.float32[3] = 1.0f;
	second.colour.float32[2] = 0.6f;
	second.colour.float32[3] = 1.0f;

	/* The surface queries, then the swapchain of head 0 and its steps. */
	step_caps2(&state, &first);
	step_create(&state, &first);
	if (first.swapchain != VK_NULL_HANDLE) {
		step_release(&state, &first);
		step_unenqueued(&state, &first);
		step_frames(&state, &first, "frames", TEST_FRAMES, 1U);
		step_pace(&state, &first);
	}

	/* Head 1's swapchain, blocked, while head 0's presents. */
	error = test_swapchain(&state, &second, NULL, 0U);
	if (error != VK_SUCCESS) {
		test_say(&state, "narrow: FAIL swapchain of head 1 %d", (int)error);
		state.failed = 1;
	} else if (first.swapchain != VK_NULL_HANDLE) {
		step_narrow(&state, &first, &second);
	}

	/* Head 0's swapchain is destroyed with presents queued. */
	if (first.swapchain != VK_NULL_HANDLE)
		step_destroy(&state, &first);

	/* The head change last. */
	if (second.swapchain != VK_NULL_HANDLE)
		step_unplug(&state, &second);

	/* The summary; the objects are left to the process's exit. */
	if (state.failed == 0) {
		test_say(&state, "present-fence-test: PASS");
	} else {
		test_say(&state, "present-fence-test: FAIL");
	}
	snprintf(path, sizeof(path), "%s/done", state.directory);
	done = fopen(path, "w");
	if (done != NULL)
		fclose(done);
	_exit(state.failed);
}

/* Makes the instance with the surface maintenance, the device with the swapchain maintenance, and the clear commands. */
static int
test_setup(
	struct test_state *state)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_create;
	VkDeviceCreateInfo device_create;
	VkDeviceQueueCreateInfo queue_create;
	VkCommandPoolCreateInfo pool_create;
	VkCommandBufferAllocateInfo commands_create;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT feature;
	VkQueueFamilyProperties families[8];
	VkDisplayPropertiesKHR properties[TEST_DISPLAYS];
	const char *instance_names[5];
	const char *device_names[2];
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult error;

	/* The direct-display instance with the extensible queries swapchain maintenance needs. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "present-fence-test";
	application.apiVersion = VK_API_VERSION_1_0;
	instance_names[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	instance_names[1] = VK_KHR_DISPLAY_EXTENSION_NAME;
	instance_names[2] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
	instance_names[3] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
	instance_names[4] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
	memset(&instance_create, 0, sizeof(instance_create));
	instance_create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_create.pApplicationInfo = &application;
	instance_create.enabledExtensionCount = 5U;
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

	/* The first family with graphics clears the images. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, families);
	for (index = 0U; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U)
			break;
	}
	if (index == count) {
		test_say(state, "setup: no graphics family");
		return 12;
	}
	state->family = index;

	/* The device with the swapchain and its maintenance, the feature enabled. */
	memset(&feature, 0, sizeof(feature));
	feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
	feature.swapchainMaintenance1 = VK_TRUE;
	priority = 1.0f;
	memset(&queue_create, 0, sizeof(queue_create));
	queue_create.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_create.queueFamilyIndex = state->family;
	queue_create.queueCount = 1U;
	queue_create.pQueuePriorities = &priority;
	device_names[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	device_names[1] = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
	memset(&device_create, 0, sizeof(device_create));
	device_create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_create.pNext = &feature;
	device_create.queueCreateInfoCount = 1U;
	device_create.pQueueCreateInfos = &queue_create;
	device_create.enabledExtensionCount = 2U;
	device_create.ppEnabledExtensionNames = device_names;
	error = vkCreateDevice(state->physical, &device_create, NULL, &state->device);
	if (error != VK_SUCCESS) {
		test_say(state, "setup: device error=%d", (int)error);
		return 13;
	}
	vkGetDeviceQueue(state->device, state->family, 0U, &state->queue);

	/* One command buffer clears every image; one fence waits for acquisitions and clears. */
	memset(&pool_create, 0, sizeof(pool_create));
	pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_create.queueFamilyIndex = state->family;
	error = vkCreateCommandPool(state->device, &pool_create, NULL, &state->pool);
	if (error != VK_SUCCESS)
		return 14;
	memset(&commands_create, 0, sizeof(commands_create));
	commands_create.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commands_create.commandPool = state->pool;
	commands_create.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commands_create.commandBufferCount = 1U;
	error = vkAllocateCommandBuffers(state->device, &commands_create, &state->commands);
	if (error != VK_SUCCESS)
		return 15;
	error = test_fence(state, &state->done);
	if (error != VK_SUCCESS)
		return 16;

	/* The displays connected now. */
	count = TEST_DISPLAYS;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR(state->physical, &count, properties);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		count = 0U;
	for (index = 0U; index < count; index++)
		state->displays[index] = properties[index].display;
	state->display_count = count;

	/* Succeeded: the device can present and clear. */
	return 0;
}

/* Writes one result line. */
static void
test_say(
	struct test_state *state,
	const char *format,
	...)
{
	va_list arguments;

	/* A failing step marks the whole run failed. */
	va_start(arguments, format);
	vfprintf(state->result, format, arguments);
	va_end(arguments);
	fputc('\n', state->result);
	fflush(state->result);
	if (strstr(format, ": FAIL") != NULL)
		state->failed = 1;
}

/* Makes the marker file the host waits for before it changes a head. */
static void
test_marker(
	struct test_state *state,
	const char *name)
{
	char path[256];
	FILE *marker;

	/* An empty file is the marker. */
	snprintf(path, sizeof(path), "%s/%s.armed", state->directory, name);
	marker = fopen(path, "w");
	if (marker != NULL)
		fclose(marker);
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
test_now_ms(
	void)
{
	struct timespec now;

	/* The monotonic clock, which the steps' times are differences of. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* The milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Sleeps for some milliseconds. */
static void
test_sleep_ms(
	unsigned milliseconds)
{
	struct timespec delay;

	/* One sleep, interrupted or not. */
	delay.tv_sec = milliseconds / 1000U;
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	nanosleep(&delay, NULL);
}

/* Reports whether an extension is in an enumeration. */
static int
test_has_extension(
	const VkExtensionProperties *properties,
	uint32_t count,
	const char *name)
{
	uint32_t index;
	int match;

	/* Looks for the name. */
	for (index = 0U; index < count; index++) {
		match = strcmp(properties[index].extensionName, name);
		if (match == 0)
			return 1;
	}

	/* Not offered. */
	return 0;
}

/* Makes a surface on a display's first mode, on a plane no other surface of the run has. */
static VkResult
test_surface(
	struct test_state *state,
	uint32_t index,
	struct test_output *output)
{
	VkDisplayModePropertiesKHR modes[8];
	VkDisplaySurfaceCreateInfoKHR surface_create;
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[8];
	VkDisplayKHR supported[TEST_DISPLAYS];
	uint32_t plane_count;
	uint32_t plane;
	uint32_t count;
	uint32_t at;
	int found;
	VkResult error;

	/* The display's first mode decides the extent and the refresh. */
	output->display = state->displays[index];
	count = sizeof(modes) / sizeof(modes[0]);
	error = vkGetDisplayModePropertiesKHR(state->physical, output->display, &count, modes);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	output->extent = modes[0].parameters.visibleRegion;
	output->refresh_mhz = modes[0].parameters.refreshRate;

	/* The first plane not used yet that drives the display. */
	plane_count = 0U;
	error = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(state->physical, &plane_count, NULL);
	if (error != VK_SUCCESS)
		return error;
	found = 0;
	plane = 0U;
	for (plane = 0U; plane < plane_count && plane < 32U && found == 0; plane++) {
		if ((state->plane_used & (1U << plane)) != 0U)
			continue;
		count = TEST_DISPLAYS;
		error = vkGetDisplayPlaneSupportedDisplaysKHR(state->physical, plane, &count, supported);
		if (error != VK_SUCCESS && error != VK_INCOMPLETE)
			return error;
		for (at = 0U; at < count; at++) {
			if (supported[at] == output->display)
				found = 1;
		}
		if (found != 0)
			break;
	}
	if (found == 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	state->plane_used |= 1U << plane;

	/* A full-display opaque surface on the plane. */
	memset(&surface_create, 0, sizeof(surface_create));
	surface_create.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	surface_create.displayMode = modes[0].displayMode;
	surface_create.planeIndex = plane;
	surface_create.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	surface_create.globalAlpha = 1.0f;
	surface_create.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	surface_create.imageExtent = output->extent;
	error = vkCreateDisplayPlaneSurfaceKHR(state->instance, &surface_create, NULL, &output->surface);
	if (error != VK_SUCCESS)
		return error;

	/* The surface's image count and its first format. */
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(state->physical, output->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;
	output->minimum = capabilities.minImageCount;
	count = sizeof(formats) / sizeof(formats[0]);
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(state->physical, output->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	output->format = formats[0];

	/* Succeeded: the display has a surface of its own. */
	return VK_SUCCESS;
}

/* Makes a FIFO swapchain on an output's surface, with the creation records given. */
static VkResult
test_swapchain(
	struct test_state *state,
	struct test_output *output,
	const void *next,
	VkSwapchainCreateFlagsKHR flags)
{
	VkSwapchainCreateInfoKHR swapchain_create;
	VkResult error;

	/* A FIFO swapchain whose images are cleared by transfers. */
	memset(&swapchain_create, 0, sizeof(swapchain_create));
	swapchain_create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_create.pNext = next;
	swapchain_create.flags = flags;
	swapchain_create.surface = output->surface;
	swapchain_create.minImageCount = output->minimum;
	swapchain_create.imageFormat = output->format.format;
	swapchain_create.imageColorSpace = output->format.colorSpace;
	swapchain_create.imageExtent = output->extent;
	swapchain_create.imageArrayLayers = 1U;
	swapchain_create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapchain_create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	swapchain_create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchain_create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchain_create.clipped = VK_TRUE;
	error = vkCreateSwapchainKHR(state->device, &swapchain_create, NULL, &output->swapchain);
	if (error != VK_SUCCESS) {
		output->swapchain = VK_NULL_HANDLE;
		return error;
	}

	/* The swapchain's images. */
	output->image_count = TEST_IMAGES;
	error = vkGetSwapchainImagesKHR(state->device, output->swapchain, &output->image_count, output->images);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the output has a swapchain. */
	return VK_SUCCESS;
}

/* Makes an unsignaled fence. */
static VkResult
test_fence(
	struct test_state *state,
	VkFence *fence)
{
	VkFenceCreateInfo fence_create;
	VkResult error;

	/* An ordinary fence, unsignaled. */
	memset(&fence_create, 0, sizeof(fence_create));
	fence_create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	error = vkCreateFence(state->device, &fence_create, NULL, fence);

	/* Reports what the creation reported. */
	return error;
}

/* Acquires an image and waits for the acquisition's fence. */
static VkResult
test_acquire_image(
	struct test_state *state,
	struct test_output *output,
	uint64_t timeout,
	uint32_t *image)
{
	VkResult error;
	VkResult waited;

	/* The next image, with the fence saying it is free. */
	error = vkAcquireNextImageKHR(state->device, output->swapchain, timeout, VK_NULL_HANDLE, state->done, image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;
	waited = vkWaitForFences(state->device, 1U, &state->done, VK_TRUE, 5U * TEST_SECOND_NS);
	if (waited != VK_SUCCESS)
		return waited;
	waited = vkResetFences(state->device, 1U, &state->done);
	if (waited != VK_SUCCESS)
		return waited;

	/* Reports the acquisition's result. */
	return error;
}

/* Clears an image to the output's colour and leaves it ready to present. */
static VkResult
test_clear(
	struct test_state *state,
	struct test_output *output,
	uint32_t image)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkImageSubresourceRange range;
	VkSubmitInfo submit;
	VkResult error;

	/* The whole colour image. */
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1U;
	range.layerCount = 1U;

	/* Records the clear between the two layout transitions. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	error = vkBeginCommandBuffer(state->commands, &begin);
	if (error != VK_SUCCESS)
		return error;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = output->images[image];
	barrier.subresourceRange = range;
	vkCmdPipelineBarrier(state->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	vkCmdClearColorImage(state->commands, output->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &output->colour, 1U, &range);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0U;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(state->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	error = vkEndCommandBuffer(state->commands);
	if (error != VK_SUCCESS)
		return error;

	/* Runs the clear and waits for it, so the command buffer is free again. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &state->commands;
	error = vkQueueSubmit(state->queue, 1U, &submit, state->done);
	if (error != VK_SUCCESS)
		return error;
	error = vkWaitForFences(state->device, 1U, &state->done, VK_TRUE, 5U * TEST_SECOND_NS);
	if (error != VK_SUCCESS)
		return error;
	error = vkResetFences(state->device, 1U, &state->done);

	/* Reports the reset's result. */
	return error;
}

/* Presents an image with a present fence (or none) and the swapchain's own mode named. */
static VkResult
test_present(
	struct test_state *state,
	struct test_output *output,
	uint32_t image,
	VkFence fence,
	VkSemaphore wait)
{
	VkPresentInfoKHR present;
	VkSwapchainPresentFenceInfoEXT fences;
	VkSwapchainPresentModeInfoEXT modes;
	VkPresentModeKHR mode;
	VkResult error;

	/* The mode record, and the fence record ahead of it when there is a fence. */
	mode = VK_PRESENT_MODE_FIFO_KHR;
	memset(&modes, 0, sizeof(modes));
	modes.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT;
	modes.swapchainCount = 1U;
	modes.pPresentModes = &mode;
	memset(&fences, 0, sizeof(fences));
	fences.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
	fences.pNext = &modes;
	fences.swapchainCount = 1U;
	fences.pFences = &fence;

	/* The present itself. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext = &modes;
	if (fence != VK_NULL_HANDLE)
		present.pNext = &fences;
	if (wait != VK_NULL_HANDLE) {
		present.waitSemaphoreCount = 1U;
		present.pWaitSemaphores = &wait;
	}
	present.swapchainCount = 1U;
	present.pSwapchains = &output->swapchain;
	present.pImageIndices = &image;
	error = vkQueuePresentKHR(state->queue, &present);

	/* Reports what the present reported. */
	return error;
}

/* Prepares a waiter for a run of at most capacity presents. */
static int
test_waiter_init(
	struct test_waiter *waiter,
	VkDevice device,
	uint32_t capacity)
{
	/* The fences and their signal times, one a present. */
	memset(waiter, 0, sizeof(*waiter));
	pthread_mutex_init(&waiter->lock, NULL);
	pthread_cond_init(&waiter->cond, NULL);
	waiter->device = device;
	waiter->capacity = capacity;
	waiter->fences = calloc(capacity, sizeof(*waiter->fences));
	waiter->signaled_ms = calloc(capacity, sizeof(*waiter->signaled_ms));
	if (waiter->fences == NULL || waiter->signaled_ms == NULL)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Hands the fence of an enqueued present to the waiter. */
static void
test_waiter_add(
	struct test_waiter *waiter,
	VkFence fence)
{
	/* The fence goes last; the waiter wakes for it only when it had nothing to wait for. */
	pthread_mutex_lock(&waiter->lock);

	waiter->fences[waiter->submitted] = fence;
	waiter->submitted++;
	pthread_cond_broadcast(&waiter->cond);

	pthread_mutex_unlock(&waiter->lock);
}

/* Tells the waiter that no more presents come. */
static void
test_waiter_finish(
	struct test_waiter *waiter)
{
	/* The waiter ends once it saw every fence. */
	pthread_mutex_lock(&waiter->lock);

	waiter->finished = 1;
	pthread_cond_broadcast(&waiter->cond);

	pthread_mutex_unlock(&waiter->lock);
}

/* Waits up to a limit for the waiter to see every fence; 0 when it did, -1 when it is still waiting. */
static int
test_waiter_join(
	struct test_waiter *waiter,
	pthread_t thread,
	unsigned limit_ms)
{
	struct timespec deadline;
	int exited;

	/* The deadline on the clock the condition uses. */
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += limit_ms / 1000U;
	deadline.tv_nsec += (long)(limit_ms % 1000U) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	/* The waiter says it is done; a waiter still blocked is left behind. */
	pthread_mutex_lock(&waiter->lock);

	while (waiter->exited == 0) {
		if (pthread_cond_timedwait(&waiter->cond, &waiter->lock, &deadline) == ETIMEDOUT)
			break;
	}
	exited = waiter->exited;

	pthread_mutex_unlock(&waiter->lock);

	/* A blocked waiter is reported and not joined. */
	if (exited == 0)
		return -1;
	pthread_join(thread, NULL);

	/* Succeeded: the waiter saw every fence. */
	return 0;
}

/*
 * Waits for the fences of the presents, oldest first, with any-waits
 * without a limit: each wake notes the time of every fence now signaled and
 * checks that no fence signaled while an older one had not.
 */
static void *
test_waiter_main(
	void *argument)
{
	struct test_waiter *waiter;
	VkFence *pending;
	uint32_t first;
	uint32_t last;
	uint32_t index;
	uint32_t newly;
	int gap;
	uint64_t now;
	VkResult status;
	VkResult error;

	/* Each pass: the fences not seen yet, then one wait for any of them. */
	waiter = argument;
	for (;;) {
		pthread_mutex_lock(&waiter->lock);

		while (waiter->observed == waiter->submitted && waiter->finished == 0)
			pthread_cond_wait(&waiter->cond, &waiter->lock);
		first = waiter->observed;
		last = waiter->submitted;
		pending = waiter->fences;

		pthread_mutex_unlock(&waiter->lock);

		/* Every fence seen and no more to come. */
		if (first == last)
			break;

		/* Any of the fences not seen yet. */
		error = vkWaitForFences(waiter->device, last - first, &pending[first], VK_FALSE, UINT64_MAX);
		now = test_now_ms();
		if (error != VK_SUCCESS) {
			waiter->error = error;
			break;
		}

		/* The signaled ones from the oldest on; a signaled one after an unsignaled one is out of order. */
		newly = 0U;
		gap = 0;
		for (index = first; index < last; index++) {
			status = vkGetFenceStatus(waiter->device, pending[index]);
			if (status == VK_SUCCESS && gap != 0) {
				waiter->out_of_order++;
			} else if (status == VK_SUCCESS) {
				waiter->signaled_ms[index] = now;
				newly++;
			} else {
				gap = 1;
			}
		}

		/* The seen ones are done with. */
		pthread_mutex_lock(&waiter->lock);

		waiter->observed = first + newly;

		pthread_mutex_unlock(&waiter->lock);
	}

	/* Says it ended. */
	pthread_mutex_lock(&waiter->lock);

	waiter->exited = 1;
	pthread_cond_broadcast(&waiter->cond);

	pthread_mutex_unlock(&waiter->lock);

	/* Nothing to report but the waiter's fields. */
	return NULL;
}

/* Acquires once with the measured thread's polls counted. */
static void *
test_acquire_main(
	void *argument)
{
	struct test_acquire *acquire;
	uint32_t image;

	/* This thread's blocking polls are the ones counted. */
	acquire = argument;
	counted_thread = pthread_self();
	__atomic_store_n(&counted_polls, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&counting, 1, __ATOMIC_RELEASE);
	acquire->result = vkAcquireNextImageKHR(acquire->state->device, acquire->output->swapchain, acquire->timeout,
	    VK_NULL_HANDLE, acquire->fence, &image);
	__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
	acquire->polls = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);

	/* Nothing to report but the acquisition's fields. */
	return NULL;
}

/* The extensions are offered and the feature is reported. */
static void
step_ext(
	struct test_state *state)
{
	VkExtensionProperties properties[64];
	VkPhysicalDeviceFeatures2 features;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT feature;
	uint32_t count;
	int capabilities2;
	int surface;
	int swapchain;

	/* The instance extensions. */
	count = 64U;
	vkEnumerateInstanceExtensionProperties(NULL, &count, properties);
	capabilities2 = test_has_extension(properties, count, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	surface = test_has_extension(properties, count, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

	/* The device extension. */
	count = 64U;
	vkEnumerateDeviceExtensionProperties(state->physical, NULL, &count, properties);
	swapchain = test_has_extension(properties, count, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);

	/* The feature. */
	memset(&feature, 0, sizeof(feature));
	feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
	memset(&features, 0, sizeof(features));
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &feature;
	vkGetPhysicalDeviceFeatures2KHR(state->physical, &features);

	/* All of them, and the device was made with them (setup). */
	if (capabilities2 && surface && swapchain && feature.swapchainMaintenance1 == VK_TRUE) {
		test_say(state, "ext: PASS");
	} else {
		test_say(state, "ext: FAIL caps2=%d surface=%d swapchain=%d feature=%u", capabilities2, surface, swapchain,
		    (unsigned)feature.swapchainMaintenance1);
	}
}

/* The extensible surface queries and the surface maintenance records. */
static void
step_caps2(
	struct test_state *state,
	struct test_output *output)
{
	VkPhysicalDeviceSurfaceInfo2KHR info;
	VkSurfacePresentModeEXT mode;
	VkSurfaceCapabilities2KHR capabilities;
	VkSurfacePresentScalingCapabilitiesEXT scaling;
	VkSurfacePresentModeCompatibilityEXT compatibility;
	VkPresentModeKHR compatible[4];
	VkSurfaceFormatKHR formats[8];
	VkSurfaceFormat2KHR formats2[8];
	uint32_t count;
	uint32_t count2;
	uint32_t short_count;
	uint32_t index;
	uint32_t compatible_count;
	VkResult error;
	VkResult counted;
	VkResult shortened;
	int same;

	/* The question names the surface and FIFO; the answer carries both maintenance records. */
	memset(&mode, 0, sizeof(mode));
	mode.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT;
	mode.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
	info.pNext = &mode;
	info.surface = output->surface;
	memset(&compatibility, 0, sizeof(compatibility));
	compatibility.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT;
	memset(&scaling, 0, sizeof(scaling));
	scaling.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT;
	scaling.pNext = &compatibility;
	scaling.supportedPresentScaling = 0x7U;
	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
	capabilities.pNext = &scaling;

	/* The count of compatible modes first, then the modes. */
	error = vkGetPhysicalDeviceSurfaceCapabilities2KHR(state->physical, &info, &capabilities);
	compatible_count = compatibility.presentModeCount;
	compatibility.pPresentModes = compatible;
	compatibility.presentModeCount = 4U;
	compatible[0] = VK_PRESENT_MODE_MAILBOX_KHR;
	if (error == VK_SUCCESS)
		error = vkGetPhysicalDeviceSurfaceCapabilities2KHR(state->physical, &info, &capabilities);

	/* The formats through both queries, and through a one-entry array. */
	count = 8U;
	counted = vkGetPhysicalDeviceSurfaceFormatsKHR(state->physical, output->surface, &count, formats);
	count2 = 0U;
	vkGetPhysicalDeviceSurfaceFormats2KHR(state->physical, &info, &count2, NULL);
	memset(formats2, 0, sizeof(formats2));
	for (index = 0U; index < 8U; index++)
		formats2[index].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
	if (count2 > 8U)
		count2 = 8U;
	vkGetPhysicalDeviceSurfaceFormats2KHR(state->physical, &info, &count2, formats2);
	same = (counted == VK_SUCCESS && count2 == count);
	for (index = 0U; index < count && same; index++) {
		if (formats2[index].surfaceFormat.format != formats[index].format ||
		    formats2[index].surfaceFormat.colorSpace != formats[index].colorSpace)
			same = 0;
	}
	short_count = 1U;
	shortened = vkGetPhysicalDeviceSurfaceFormats2KHR(state->physical, &info, &short_count, formats2);
	if (count < 2U)
		shortened = VK_INCOMPLETE;

	/* FIFO alone, no scaling, the same formats, and a short array told so. */
	if (error == VK_SUCCESS &&
	    capabilities.surfaceCapabilities.minImageCount == output->minimum &&
	    compatible_count == 1U &&
	    compatibility.presentModeCount == 1U &&
	    compatible[0] == VK_PRESENT_MODE_FIFO_KHR &&
	    scaling.supportedPresentScaling == 0U &&
	    same &&
	    shortened == VK_INCOMPLETE) {
		test_say(state, "caps2: PASS formats=%u", count);
	} else {
		test_say(state, "caps2: FAIL error=%d min=%u compatible=%u/%u mode=%d scaling=%u formats=%u/%u same=%d short=%d",
		    (int)error, capabilities.surfaceCapabilities.minImageCount, compatible_count, compatibility.presentModeCount,
		    (int)compatible[0], scaling.supportedPresentScaling, count, count2, same, (int)shortened);
	}
}

/* A swapchain naming other modes is refused; one naming its own, no scaling and a deferred allocation is made. */
static void
step_create(
	struct test_state *state,
	struct test_output *output)
{
	VkSwapchainPresentModesCreateInfoEXT modes;
	VkSwapchainPresentScalingCreateInfoEXT scaling;
	VkPresentModeKHR list[2];
	VkResult refused;
	VkResult made;

	/* FIFO and MAILBOX: a switch the swapchain does not make. */
	list[0] = VK_PRESENT_MODE_FIFO_KHR;
	list[1] = VK_PRESENT_MODE_MAILBOX_KHR;
	memset(&modes, 0, sizeof(modes));
	modes.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT;
	modes.presentModeCount = 2U;
	modes.pPresentModes = list;
	refused = test_swapchain(state, output, &modes, 0U);
	if (refused == VK_SUCCESS) {
		vkDestroySwapchainKHR(state->device, output->swapchain, NULL);
		output->swapchain = VK_NULL_HANDLE;
	}

	/* FIFO alone, no scaling, a deferred allocation. */
	memset(&scaling, 0, sizeof(scaling));
	scaling.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT;
	modes.pNext = &scaling;
	modes.presentModeCount = 1U;
	made = test_swapchain(state, output, &modes, VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT);

	/* The refusal and the swapchain. */
	if (refused == VK_ERROR_INITIALIZATION_FAILED && made == VK_SUCCESS) {
		test_say(state, "create: PASS images=%u", output->image_count);
	} else {
		test_say(state, "create: FAIL refused=%d made=%d", (int)refused, (int)made);
	}
}

/* An acquired image released is the one acquired again; a second release is refused. */
static void
step_release(
	struct test_state *state,
	struct test_output *output)
{
	VkReleaseSwapchainImagesInfoEXT release;
	uint32_t held[TEST_IMAGES];
	uint32_t count;
	uint32_t index;
	uint32_t again;
	VkResult acquired;
	VkResult released;
	VkResult twice;
	VkResult reacquired;
	VkResult rest;

	/* Every image held, then the first released unpresented. */
	count = 0U;
	acquired = VK_SUCCESS;
	for (index = 0U; index < output->image_count && acquired == VK_SUCCESS; index++) {
		acquired = test_acquire_image(state, output, TEST_SECOND_NS, &held[count]);
		if (acquired == VK_SUCCESS)
			count++;
	}
	memset(&release, 0, sizeof(release));
	release.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT;
	release.swapchain = output->swapchain;
	release.imageIndexCount = 1U;
	release.pImageIndices = &held[0];
	released = VK_ERROR_UNKNOWN;
	twice = VK_ERROR_UNKNOWN;
	if (count != 0U) {
		released = vkReleaseSwapchainImagesEXT(state->device, &release);
		twice = vkReleaseSwapchainImagesEXT(state->device, &release);
	}

	/* Without waiting, the released image is the only one to acquire. */
	again = UINT32_MAX;
	reacquired = test_acquire_image(state, output, 0U, &again);
	if (reacquired == VK_SUCCESS && count != 0U && again != held[0])
		reacquired = VK_ERROR_UNKNOWN;

	/* Every held image goes back for the next steps. */
	release.imageIndexCount = count;
	release.pImageIndices = held;
	rest = vkReleaseSwapchainImagesEXT(state->device, &release);

	/* Released once, refused the second time, the same image acquired again. */
	if (acquired == VK_SUCCESS &&
	    count == output->image_count &&
	    released == VK_SUCCESS &&
	    twice == VK_ERROR_INITIALIZATION_FAILED &&
	    reacquired == VK_SUCCESS &&
	    rest == VK_SUCCESS) {
		test_say(state, "release: PASS images=%u again=%u", count, again);
	} else {
		test_say(state, "release: FAIL acquired=%d count=%u released=%d twice=%d reacquired=%d again=%u rest=%d", (int)acquired,
		    count, (int)released, (int)twice, (int)reacquired, again, (int)rest);
	}
}

/* A refused present leaves its fence unsignaled. */
static void
step_unenqueued(
	struct test_state *state,
	struct test_output *output)
{
	VkFence fence;
	VkResult presented;
	VkResult status;

	/* Image 0 is not acquired (the release step gave every image back). */
	if (test_fence(state, &fence) != VK_SUCCESS) {
		test_say(state, "unenqueued: FAIL fence");
		return;
	}
	presented = test_present(state, output, 0U, fence, VK_NULL_HANDLE);
	test_sleep_ms(200U);
	status = vkGetFenceStatus(state->device, fence);
	vkDestroyFence(state->device, fence, NULL);

	/* Refused, and the fence unchanged. */
	if (presented == VK_ERROR_INITIALIZATION_FAILED && status == VK_NOT_READY) {
		test_say(state, "unenqueued: PASS");
	} else {
		test_say(state, "unenqueued: FAIL presented=%d status=%d", (int)presented, (int)status);
	}
}

/* Sorts some intervals and returns their median. */
static uint64_t
test_median(
	uint64_t *values,
	uint32_t count)
{
	uint64_t swap;
	uint32_t index;
	uint32_t at;

	/* No value has no median. */
	if (count == 0U)
		return 0U;

	/* A plain sort; the runs are short. */
	for (index = 0U; index < count; index++) {
		for (at = index + 1U; at < count; at++) {
			if (values[at] < values[index]) {
				swap = values[at];
				values[at] = values[index];
				values[index] = swap;
			}
		}
	}

	/* The middle one. */
	return values[count / 2U];
}

/*
 * Presents frames with fences another thread waits for, and checks them.
 *
 * The images are cleared once and given back; each frame then only acquires
 * with a semaphore the present waits for, so the presents come as fast as
 * the swapchain gives images back and FIFO, not the program, sets the pace.
 */
static void
step_frames(
	struct test_state *state,
	struct test_output *output,
	const char *name,
	uint32_t frames,
	uint32_t check_rate)
{
	VkReleaseSwapchainImagesInfoEXT release;
	VkSemaphoreCreateInfo semaphore_create;
	struct test_waiter waiter;
	pthread_t thread;
	VkFence *fences;
	VkSemaphore *semaphores;
	uint64_t *presented_ms;
	uint64_t *intervals;
	uint64_t *latencies;
	uint64_t period_us;
	uint64_t median;
	uint64_t latency;
	uint64_t producer;
	uint32_t held[TEST_IMAGES];
	uint32_t frame;
	uint32_t image;
	uint32_t count;
	uint32_t index;
	VkResult error;
	int joined;

	/* The fences and semaphores of every frame, made before the presents. */
	fences = calloc(frames, sizeof(*fences));
	semaphores = calloc(frames, sizeof(*semaphores));
	presented_ms = calloc(frames, sizeof(*presented_ms));
	intervals = calloc(frames, sizeof(*intervals));
	latencies = calloc(frames, sizeof(*latencies));
	if (fences == NULL || semaphores == NULL || presented_ms == NULL || intervals == NULL || latencies == NULL) {
		test_say(state, "%s: FAIL memory", name);
		return;
	}
	memset(&semaphore_create, 0, sizeof(semaphore_create));
	semaphore_create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	error = VK_SUCCESS;
	for (frame = 0U; frame < frames && error == VK_SUCCESS; frame++) {
		error = test_fence(state, &fences[frame]);
		if (error == VK_SUCCESS)
			error = vkCreateSemaphore(state->device, &semaphore_create, NULL, &semaphores[frame]);
	}

	/*
	 * Every image cleared once, then given back unpresented.  Only before
	 * the swapchain's first present: after it, the output shows an image
	 * until the next present, so not every image can be held at once.
	 */
	count = 0U;
	for (index = 0U; index < output->image_count && error == VK_SUCCESS && output->cleared == 0; index++) {
		error = test_acquire_image(state, output, TEST_SECOND_NS, &held[count]);
		if (error == VK_SUCCESS)
			error = test_clear(state, output, held[count]);
		if (error == VK_SUCCESS)
			count++;
	}
	memset(&release, 0, sizeof(release));
	release.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT;
	release.swapchain = output->swapchain;
	release.imageIndexCount = count;
	release.pImageIndices = held;
	if (count != 0U)
		vkReleaseSwapchainImagesEXT(state->device, &release);
	if (error != VK_SUCCESS) {
		test_say(state, "%s: FAIL preparing %d", name, (int)error);
		return;
	}
	output->cleared = 1;

	/* The waiter and its thread. */
	if (test_waiter_init(&waiter, state->device, frames) != 0 ||
	    pthread_create(&thread, NULL, test_waiter_main, &waiter) != 0) {
		test_say(state, "%s: FAIL waiter", name);
		return;
	}

	/* Each frame: acquire with the frame's semaphore, present waiting for it with the frame's fence. */
	for (frame = 0U; frame < frames && error == VK_SUCCESS; frame++) {
		error = vkAcquireNextImageKHR(state->device, output->swapchain, 5U * TEST_SECOND_NS, semaphores[frame], VK_NULL_HANDLE, &image);
		if (error != VK_SUCCESS)
			break;
		presented_ms[frame] = test_now_ms();
		error = test_present(state, output, image, fences[frame], semaphores[frame]);
		test_waiter_add(&waiter, fences[frame]);
	}
	test_waiter_finish(&waiter);
	joined = test_waiter_join(&waiter, thread, 10000U);
	if (joined != 0 || error != VK_SUCCESS || waiter.observed != frames || waiter.out_of_order != 0U || waiter.error != VK_SUCCESS) {
		test_say(state, "%s: FAIL presented=%u error=%d joined=%d observed=%u out-of-order=%u wait=%d", name, frame,
		    (int)error, joined, waiter.observed, waiter.out_of_order, (int)waiter.error);
		return;
	}

	/* The medians: between two signals, from a present to its signal, and between two presents. */
	for (index = 1U; index < frames; index++)
		intervals[index - 1U] = waiter.signaled_ms[index] - waiter.signaled_ms[index - 1U];
	median = test_median(intervals, frames - 1U);
	for (index = 0U; index < frames; index++)
		latencies[index] = waiter.signaled_ms[index] - presented_ms[index];
	latency = test_median(latencies, frames);
	for (index = 1U; index < frames; index++)
		intervals[index - 1U] = presented_ms[index] - presented_ms[index - 1U];
	producer = test_median(intervals, frames - 1U);
	for (index = 0U; index < frames; index++) {
		vkDestroyFence(state->device, fences[index], NULL);
		vkDestroySemaphore(state->device, semaphores[index], NULL);
	}
	free(fences);
	free(semaphores);
	free(presented_ms);
	free(intervals);
	free(latencies);

	/*
	 * The signals follow the presents: their median interval is within a
	 * quarter of the presents' (whole milliseconds), and FIFO keeps it from
	 * being shorter than three quarters of the mode's refresh period.  How
	 * many refresh periods a present takes is the renderer's and the
	 * kernel's, which the run without fences shows.
	 */
	period_us = 0U;
	if (output->refresh_mhz != 0U)
		period_us = UINT64_C(1000000000) / output->refresh_mhz;
	if (check_rate == 0U ||
	    (period_us != 0U &&
	     median * 4U + 4U >= producer * 3U &&
	     median * 4U <= producer * 5U + 4U &&
	     median * 1000U * 4U + 4000U >= period_us * 3U)) {
		test_say(state, "%s: PASS n=%u signal-ms=%llu latency-ms=%llu present-ms=%llu period-us=%llu", name, frames,
		    (unsigned long long)median, (unsigned long long)latency, (unsigned long long)producer, (unsigned long long)period_us);
	} else {
		test_say(state, "%s: FAIL rate n=%u signal-ms=%llu latency-ms=%llu present-ms=%llu period-us=%llu", name, frames,
		    (unsigned long long)median, (unsigned long long)latency, (unsigned long long)producer, (unsigned long long)period_us);
	}
}

/*
 * Presents frames without fences, as the frames step does otherwise: the
 * pace of the presents alone, which the frames step's signals are compared
 * with in the result.
 */
static void
step_pace(
	struct test_state *state,
	struct test_output *output)
{
	VkSemaphoreCreateInfo semaphore_create;
	VkSemaphore semaphores[TEST_NARROW_FRAMES];
	uint64_t presented_ms[TEST_NARROW_FRAMES];
	uint64_t intervals[TEST_NARROW_FRAMES];
	uint32_t frame;
	uint32_t image;
	VkResult error;

	/* The semaphores of every frame. */
	memset(&semaphore_create, 0, sizeof(semaphore_create));
	semaphore_create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	error = VK_SUCCESS;
	for (frame = 0U; frame < TEST_NARROW_FRAMES && error == VK_SUCCESS; frame++)
		error = vkCreateSemaphore(state->device, &semaphore_create, NULL, &semaphores[frame]);

	/* Each frame: acquire with the frame's semaphore and present waiting for it. */
	for (frame = 0U; frame < TEST_NARROW_FRAMES && error == VK_SUCCESS; frame++) {
		error = vkAcquireNextImageKHR(state->device, output->swapchain, 5U * TEST_SECOND_NS, semaphores[frame], VK_NULL_HANDLE, &image);
		if (error != VK_SUCCESS)
			break;
		presented_ms[frame] = test_now_ms();
		error = test_present(state, output, image, VK_NULL_HANDLE, semaphores[frame]);
	}
	vkQueueWaitIdle(state->queue);

	/* The median interval between presents. */
	if (error != VK_SUCCESS) {
		test_say(state, "pace: FAIL presented=%u error=%d", frame, (int)error);
		return;
	}
	for (frame = 1U; frame < TEST_NARROW_FRAMES; frame++)
		intervals[frame - 1U] = presented_ms[frame] - presented_ms[frame - 1U];
	test_say(state, "pace: PASS n=%u present-ms=%llu (no fences)", TEST_NARROW_FRAMES,
	    (unsigned long long)test_median(intervals, TEST_NARROW_FRAMES - 1U));
	for (frame = 0U; frame < TEST_NARROW_FRAMES; frame++)
		vkDestroySemaphore(state->device, semaphores[frame], NULL);
}

/* An acquisition blocked on one swapchain sleeps in one poll while another swapchain presents. */
static void
step_narrow(
	struct test_state *state,
	struct test_output *output,
	struct test_output *other)
{
	VkReleaseSwapchainImagesInfoEXT release;
	struct test_acquire acquire;
	pthread_t thread;
	uint32_t held[TEST_IMAGES];
	uint32_t count;
	uint32_t index;
	uint64_t started;
	uint64_t ended;
	VkResult error;

	/* Every image of the other swapchain is held, so its next acquisition blocks. */
	count = 0U;
	error = VK_SUCCESS;
	for (index = 0U; index < other->image_count && error == VK_SUCCESS; index++) {
		error = test_acquire_image(state, other, TEST_SECOND_NS, &held[count]);
		if (error == VK_SUCCESS)
			count++;
	}
	if (error != VK_SUCCESS) {
		test_say(state, "narrow: FAIL holding images %d after %u", (int)error, count);
		return;
	}

	/* The blocked acquisition, 3 s long, on its own thread. */
	memset(&acquire, 0, sizeof(acquire));
	acquire.state = state;
	acquire.output = other;
	acquire.timeout = 3U * TEST_SECOND_NS;
	if (test_fence(state, &acquire.fence) != VK_SUCCESS) {
		test_say(state, "narrow: FAIL fence");
		return;
	}
	started = test_now_ms();
	if (pthread_create(&thread, NULL, test_acquire_main, &acquire) != 0) {
		test_say(state, "narrow: FAIL thread");
		return;
	}

	/* Meanwhile this swapchain presents with fences. */
	step_frames(state, output, "narrow-frames", TEST_NARROW_FRAMES, 0U);
	pthread_join(thread, NULL);
	ended = test_now_ms();

	/* The held images go back. */
	memset(&release, 0, sizeof(release));
	release.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT;
	release.swapchain = other->swapchain;
	release.imageIndexCount = count;
	release.pImageIndices = held;
	error = vkReleaseSwapchainImagesEXT(state->device, &release);

	/* A timeout after one poll. */
	if (acquire.result == VK_TIMEOUT && acquire.polls <= 1U && error == VK_SUCCESS) {
		test_say(state, "narrow: PASS polls=%u held=%u ms=%llu", acquire.polls, count, (unsigned long long)(ended - started));
	} else {
		test_say(state, "narrow: FAIL result=%d polls=%u held=%u release=%d ms=%llu", (int)acquire.result, acquire.polls,
		    count, (int)error, (unsigned long long)(ended - started));
	}
}

/* The fences of presents still queued are signaled once the swapchain is destroyed. */
static void
step_destroy(
	struct test_state *state,
	struct test_output *output)
{
	VkFence fences[TEST_DESTROY_FRAMES];
	VkResult statuses[TEST_DESTROY_FRAMES];
	uint32_t presented;
	uint32_t image;
	uint32_t index;
	uint32_t signaled;
	VkResult error;

	/* A few presents, back to back, nothing waited for. */
	presented = 0U;
	error = VK_SUCCESS;
	for (index = 0U; index < TEST_DESTROY_FRAMES && error == VK_SUCCESS; index++) {
		error = test_acquire_image(state, output, 5U * TEST_SECOND_NS, &image);
		if (error == VK_SUCCESS)
			error = test_fence(state, &fences[index]);
		if (error == VK_SUCCESS)
			error = test_present(state, output, image, fences[index], VK_NULL_HANDLE);
		if (error == VK_SUCCESS)
			presented++;
	}

	/* The destruction, then each fence's state at once. */
	vkDestroySwapchainKHR(state->device, output->swapchain, NULL);
	output->swapchain = VK_NULL_HANDLE;
	signaled = 0U;
	for (index = 0U; index < presented; index++) {
		statuses[index] = vkGetFenceStatus(state->device, fences[index]);
		if (statuses[index] == VK_SUCCESS)
			signaled++;
		vkDestroyFence(state->device, fences[index], NULL);
	}

	/* Every one signaled. */
	if (presented == TEST_DESTROY_FRAMES && signaled == presented) {
		test_say(state, "destroy: PASS presented=%u signaled=%u", presented, signaled);
	} else {
		test_say(state, "destroy: FAIL presented=%u signaled=%u error=%d", presented, signaled, (int)error);
	}
}

/* While the swapchain presents, the host unplugs its head: every enqueued present's fence still signals. */
static void
step_unplug(
	struct test_state *state,
	struct test_output *output)
{
	struct test_waiter waiter;
	pthread_t thread;
	uint64_t armed;
	uint64_t ended;
	uint32_t frame;
	uint32_t image;
	VkFence fence;
	VkResult error;
	VkResult presented;
	int joined;

	/* The waiter and its thread. */
	if (test_waiter_init(&waiter, state->device, TEST_UNPLUG_FRAMES) != 0 ||
	    pthread_create(&thread, NULL, test_waiter_main, &waiter) != 0) {
		test_say(state, "unplug: FAIL waiter");
		return;
	}

	/* Presents until a present or an acquisition fails; the host unplugs the head 2 s after the marker. */
	test_marker(state, "unplug");
	armed = test_now_ms();
	error = VK_SUCCESS;
	presented = VK_SUCCESS;
	for (frame = 0U; frame < TEST_UNPLUG_FRAMES; frame++) {
		error = test_acquire_image(state, output, 2U * TEST_SECOND_NS, &image);
		if (error != VK_SUCCESS)
			break;
		error = test_clear(state, output, image);
		if (error != VK_SUCCESS)
			break;
		error = test_fence(state, &fence);
		if (error != VK_SUCCESS)
			break;
		presented = test_present(state, output, image, fence, VK_NULL_HANDLE);

		/* Every present but a refused one is enqueued, an out-of-date or lost one too. */
		if (presented == VK_SUCCESS ||
		    presented == VK_SUBOPTIMAL_KHR ||
		    presented == VK_ERROR_OUT_OF_DATE_KHR ||
		    presented == VK_ERROR_SURFACE_LOST_KHR)
			test_waiter_add(&waiter, fence);
		if (presented != VK_SUCCESS)
			break;
	}
	ended = test_now_ms();
	test_waiter_finish(&waiter);
	joined = test_waiter_join(&waiter, thread, 10000U);

	/* The presents ended in a failure, and the waiter saw every enqueued fence. */
	if (frame < TEST_UNPLUG_FRAMES && joined == 0 && waiter.observed == waiter.submitted &&
	    waiter.out_of_order == 0U && waiter.error == VK_SUCCESS && waiter.submitted > 0U) {
		test_say(state, "unplug: PASS presents=%u acquire=%d present=%d ms=%llu", waiter.submitted, (int)error, (int)presented,
		    (unsigned long long)(ended - armed));
	} else {
		test_say(state, "unplug: FAIL frames=%u acquire=%d present=%d joined=%d observed=%u submitted=%u out-of-order=%u wait=%d ms=%llu",
		    frame, (int)error, (int)presented, joined, waiter.observed, waiter.submitted, waiter.out_of_order, (int)waiter.error,
		    (unsigned long long)(ended - armed));
	}
}
