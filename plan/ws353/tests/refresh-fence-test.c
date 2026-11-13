/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The guest side of the refresh-fence test (qemu-refresh-fence.sh): the
 * first-pixel-out fences of VK_EXT_display_control, which the kernel signals
 * at a display's refresh boundary (GPU_DISPLAY_FENCE, REFRESH).  One run
 * makes one instance and device, shows a colour on display 0 with a FIFO
 * swapchain of its own, and runs the steps in order, writing one result line
 * a step to DIR/result.txt and a marker file DIR/NAME.armed when the host
 * must act (read the kernel's counters with the debugger, or unplug head 1):
 *
 *   refresh-fence-test DIR
 *
 *   idle0    no registration for 20 s (the host reads the kernel's counters at the start and 8 s later)
 *   frames   100 refresh fences one after another on display 0, each waited for with UINT64_MAX:
 *            every one signals within a period of its registration, on the refresh grid, each
 *            wait in one blocking poll
 *   group    4 fences registered together signal at the same boundary (an all-wait and an any-wait)
 *   reset    a signaled fence reset stays unsignaled at later boundaries
 *   cancel   20 fences destroyed before their boundary leave nothing armed
 *   noscan   a fence of display 1, which shows nothing, does not signal in 500 ms
 *   idle1    no registration for 20 s again (counters as in idle0)
 *   unplug   a fence of display 1 waited for while the host unplugs head 1 returns
 *
 * How often the waiting thread slept is counted by this program's own
 * ppoll(): libvulkan calls ppoll() through the dynamic linker, which looks
 * in the program first.  Only blocking polls of the counting thread count.
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

/* The fences of the frames, group and cancel steps. */
#define TEST_FRAMES		100U
#define TEST_GROUP		4U
#define TEST_CANCELS		20U

/* How long an idle step lasts, in milliseconds (the host reads the counters 8 s apart within it). */
#define TEST_IDLE_MS		20000U

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
	PFN_vkRegisterDisplayEventEXT register_event;
	const char *directory;
	FILE *result;
	int failed;
};

/*
 * Whether blocking polls of this program's main thread are counted, and the
 * count; only the main thread polls while counting is on.
 */
static int counting;
static unsigned counted_polls;

static int test_setup(struct test_state *state);
static void test_say(struct test_state *state, const char *format, ...);
static void test_marker(struct test_state *state, const char *name);
static uint64_t test_now_us(void);
static void test_sleep_ms(unsigned milliseconds);
static VkResult test_surface(struct test_state *state, uint32_t index, struct test_output *output);
static VkResult test_show(struct test_state *state, struct test_output *output);
static VkResult test_register(struct test_state *state, VkDisplayKHR display, VkFence *fence);
static int test_compare(const void *left, const void *right);
static void step_idle(struct test_state *state, const char *name);
static void step_frames(struct test_state *state, struct test_output *output);
static void step_group(struct test_state *state, struct test_output *output);
static void step_reset(struct test_state *state, struct test_output *output);
static void step_cancel(struct test_state *state, struct test_output *output);
static void step_noscan(struct test_state *state, VkDisplayKHR display);
static void step_unplug(struct test_state *state, VkDisplayKHR display);

/* Counts a blocking ppoll while counting is on, then waits with the system call itself. */
int
ppoll(
	struct pollfd *fds,
	nfds_t count,
	const struct timespec *timeout,
	const sigset_t *mask)
{
	intptr_t ready;
	int measured;

	/* Only a poll that may sleep counts; a zero timeout only looks. */
	measured = __atomic_load_n(&counting, __ATOMIC_ACQUIRE);
	if (measured != 0) {
		if (timeout == NULL || timeout->tv_sec != 0 || timeout->tv_nsec != 0)
			__atomic_add_fetch(&counted_polls, 1U, __ATOMIC_RELAXED);
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

/* Waits with ppoll above, so a poll is counted once. */
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
	char path[256];
	FILE *done;
	VkResult error;
	int status;

	/* The result directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: refresh-fence-test DIR\n");
		return 2;
	}
	memset(&state, 0, sizeof(state));
	memset(&first, 0, sizeof(first));
	state.directory = argv[1];
	snprintf(path, sizeof(path), "%s/result.txt", state.directory);
	state.result = fopen(path, "w");
	if (state.result == NULL)
		return 2;
	setvbuf(state.result, NULL, _IOLBF, 0);

	/* The host starts the program and returns before the GPU's open is logged on the console. */
	test_sleep_ms(3000U);

	/* The instance, the device with the display control, and the clear commands. */
	status = test_setup(&state);
	if (status != 0) {
		test_say(&state, "setup: FAIL %d", status);
		test_say(&state, "refresh-fence-test: FAIL");
		return 1;
	}
	if (state.display_count < 2U) {
		test_say(&state, "setup: FAIL displays=%u (the host gives head 1 a size first)", state.display_count);
		test_say(&state, "refresh-fence-test: FAIL");
		return 1;
	}

	/* Display 0 shows a colour, so it scans out and makes boundaries; display 1 shows nothing. */
	error = test_surface(&state, 0U, &first);
	if (error == VK_SUCCESS)
		error = test_show(&state, &first);
	if (error != VK_SUCCESS) {
		test_say(&state, "setup: FAIL show %d", (int)error);
		test_say(&state, "refresh-fence-test: FAIL");
		return 1;
	}
	test_say(&state, "setup: refresh=%u mHz", first.refresh_mhz);

	/* The steps. */
	step_idle(&state, "idle0");
	step_frames(&state, &first);
	step_group(&state, &first);
	step_reset(&state, &first);
	step_cancel(&state, &first);
	step_noscan(&state, state.displays[1]);
	step_idle(&state, "idle1");
	step_unplug(&state, state.displays[1]);

	/* The summary; the objects are left to the process's exit. */
	if (state.failed == 0) {
		test_say(&state, "refresh-fence-test: PASS");
	} else {
		test_say(&state, "refresh-fence-test: FAIL");
	}
	snprintf(path, sizeof(path), "%s/done", state.directory);
	done = fopen(path, "w");
	if (done != NULL)
		fclose(done);
	_exit(state.failed);
}

/* Makes the direct-display instance, the device with the swapchain and the display control, and the clear commands. */
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
	VkFenceCreateInfo fence_create;
	VkQueueFamilyProperties families[8];
	VkDisplayPropertiesKHR properties[TEST_DISPLAYS];
	const char *instance_names[3];
	const char *device_names[2];
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult error;

	/* The direct-display instance with the surface counters the display control depends on. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "refresh-fence-test";
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

	/* The device with the swapchain and the display control. */
	priority = 1.0f;
	memset(&queue_create, 0, sizeof(queue_create));
	queue_create.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_create.queueFamilyIndex = state->family;
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
		return 13;
	}
	vkGetDeviceQueue(state->device, state->family, 0U, &state->queue);
	state->register_event = (PFN_vkRegisterDisplayEventEXT)vkGetDeviceProcAddr(state->device, "vkRegisterDisplayEventEXT");
	if (state->register_event == NULL) {
		test_say(state, "setup: no vkRegisterDisplayEventEXT");
		return 14;
	}

	/* One command buffer clears the images; one fence waits for acquisitions and clears. */
	memset(&pool_create, 0, sizeof(pool_create));
	pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_create.queueFamilyIndex = state->family;
	error = vkCreateCommandPool(state->device, &pool_create, NULL, &state->pool);
	if (error != VK_SUCCESS)
		return 15;
	memset(&commands_create, 0, sizeof(commands_create));
	commands_create.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commands_create.commandPool = state->pool;
	commands_create.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commands_create.commandBufferCount = 1U;
	error = vkAllocateCommandBuffers(state->device, &commands_create, &state->commands);
	if (error != VK_SUCCESS)
		return 16;
	memset(&fence_create, 0, sizeof(fence_create));
	fence_create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	error = vkCreateFence(state->device, &fence_create, NULL, &state->done);
	if (error != VK_SUCCESS)
		return 17;

	/* The displays connected now. */
	count = TEST_DISPLAYS;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR(state->physical, &count, properties);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		count = 0U;
	for (index = 0U; index < count; index++)
		state->displays[index] = properties[index].display;
	state->display_count = count;

	/* Succeeded: the device can present, clear and register display events. */
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

/* Makes the marker file the host waits for before it acts. */
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

/* Reads the monotonic clock in microseconds. */
static uint64_t
test_now_us(
	void)
{
	struct timespec now;

	/* The monotonic clock, which the steps' times are differences of. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* The microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
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

/* Makes a surface on a display's first mode, on the first plane that drives it. */
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

	/* The first plane that drives the display. */
	plane_count = 0U;
	error = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(state->physical, &plane_count, NULL);
	if (error != VK_SUCCESS)
		return error;
	found = 0;
	for (plane = 0U; plane < plane_count && found == 0; plane++) {
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

	/* Succeeded: the display has a surface. */
	return VK_SUCCESS;
}

/* Makes a FIFO swapchain on the output and presents one cleared image, which stays shown. */
static VkResult
test_show(
	struct test_state *state,
	struct test_output *output)
{
	VkSwapchainCreateInfoKHR swapchain_create;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkImageSubresourceRange range;
	VkClearColorValue colour;
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	uint32_t image;
	VkResult error;

	/* A FIFO swapchain whose images are cleared by transfers. */
	memset(&swapchain_create, 0, sizeof(swapchain_create));
	swapchain_create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
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
	if (error != VK_SUCCESS)
		return error;
	output->image_count = TEST_IMAGES;
	error = vkGetSwapchainImagesKHR(state->device, output->swapchain, &output->image_count, output->images);
	if (error != VK_SUCCESS)
		return error;

	/* The next image, with the fence saying it is free. */
	error = vkAcquireNextImageKHR(state->device, output->swapchain, 5U * TEST_SECOND_NS, VK_NULL_HANDLE, state->done, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;
	error = vkWaitForFences(state->device, 1U, &state->done, VK_TRUE, 5U * TEST_SECOND_NS);
	if (error != VK_SUCCESS)
		return error;
	error = vkResetFences(state->device, 1U, &state->done);
	if (error != VK_SUCCESS)
		return error;

	/* Records a clear to a dark green between the two layout transitions. */
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1U;
	range.layerCount = 1U;
	memset(&colour, 0, sizeof(colour));
	colour.float32[1] = 0.4f;
	colour.float32[3] = 1.0f;
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
	vkCmdClearColorImage(state->commands, output->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1U, &range);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0U;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(state->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	error = vkEndCommandBuffer(state->commands);
	if (error != VK_SUCCESS)
		return error;

	/* Runs the clear and waits for it. */
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
	if (error != VK_SUCCESS)
		return error;

	/* Presents it; the display shows it until the swapchain goes. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.swapchainCount = 1U;
	present.pSwapchains = &output->swapchain;
	present.pImageIndices = &image;
	error = vkQueuePresentKHR(state->queue, &present);

	/* Lets the present reach the display before the steps. */
	test_sleep_ms(500U);

	/* Reports what the present reported. */
	return error;
}

/* Registers a first-pixel-out fence of a display. */
static VkResult
test_register(
	struct test_state *state,
	VkDisplayKHR display,
	VkFence *fence)
{
	VkDisplayEventInfoEXT event;
	VkResult error;

	/* The only display event. */
	memset(&event, 0, sizeof(event));
	event.sType = VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT;
	event.displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT;
	error = state->register_event(state->device, display, &event, NULL, fence);

	/* Reports what the registration reported. */
	return error;
}

/* Orders two unsigned 64-bit values for qsort. */
static int
test_compare(
	const void *left,
	const void *right)
{
	uint64_t a;
	uint64_t b;

	/* The smaller value first. */
	a = *(const uint64_t *)left;
	b = *(const uint64_t *)right;
	if (a < b)
		return -1;
	if (a > b)
		return 1;

	/* Equal. */
	return 0;
}

/* Holds no registration while the host reads the kernel's counters twice. */
static void
step_idle(
	struct test_state *state,
	const char *name)
{
	/* The host reads the counters at the marker and 8 s later; the judgement is the host's. */
	test_marker(state, name);
	test_sleep_ms(TEST_IDLE_MS);
	test_say(state, "%s: held no registration for %u ms", name, TEST_IDLE_MS);
}

/*
 * Waits for 100 refresh fences one after another.  Each fence signals at
 * most one period (and a little scheduling) after its registration, and the
 * signals fall on the refresh grid: each interval between two signals is a
 * whole number of periods (the registration itself, with the renderer's
 * fence creation, may take longer than the rest of a period).
 */
static void
step_frames(
	struct test_state *state,
	struct test_output *output)
{
	uint64_t registered[TEST_FRAMES];
	uint64_t times[TEST_FRAMES];
	uint64_t latencies[TEST_FRAMES];
	uint64_t costs[TEST_FRAMES];
	uint64_t period_us;
	uint64_t gap;
	uint64_t whole;
	uint64_t residual;
	uint64_t latency_max;
	uint32_t index;
	uint32_t aligned;
	uint32_t single;
	unsigned polls;
	VkFence fence;
	VkResult error;

	/* Each fence registered after the last one signaled, waited for without a limit. */
	__atomic_store_n(&counted_polls, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&counting, 1, __ATOMIC_RELEASE);
	for (index = 0U; index < TEST_FRAMES; index++) {
		error = test_register(state, output->display, &fence);
		registered[index] = test_now_us();
		if (error != VK_SUCCESS) {
			__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
			test_say(state, "frames: FAIL register %u error=%d", index, (int)error);
			return;
		}
		error = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, UINT64_MAX);
		times[index] = test_now_us();
		vkDestroyFence(state->device, fence, NULL);
		if (error != VK_SUCCESS) {
			__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
			test_say(state, "frames: FAIL wait %u error=%d", index, (int)error);
			return;
		}
	}
	__atomic_store_n(&counting, 0, __ATOMIC_RELEASE);
	polls = __atomic_load_n(&counted_polls, __ATOMIC_RELAXED);

	/* Each registration's wait, and the intervals on the refresh grid (within 3 ms of a whole number of periods). */
	period_us = 1000000000U / (uint64_t)output->refresh_mhz;
	latency_max = 0U;
	aligned = 0U;
	single = 0U;
	for (index = 0U; index < TEST_FRAMES; index++) {
		latencies[index] = times[index] - registered[index];
		if (latencies[index] > latency_max)
			latency_max = latencies[index];
		costs[index] = 0U;
		if (index == 0U)
			continue;
		costs[index] = registered[index] - times[index - 1U];
		gap = times[index] - times[index - 1U];
		whole = (gap + period_us / 2U) / period_us;
		if (whole * period_us > gap) {
			residual = whole * period_us - gap;
		} else {
			residual = gap - whole * period_us;
		}
		if (whole >= 1U && residual <= 3000U)
			aligned++;
		if (whole == 1U)
			single++;
	}
	qsort(latencies, TEST_FRAMES, sizeof(latencies[0]), test_compare);
	qsort(costs, TEST_FRAMES, sizeof(costs[0]), test_compare);

	/* Every wait within a period and 4 ms, nine intervals in ten on the grid, at most two blocking polls a fence. */
	if (latency_max > period_us + 4000U ||
	    aligned * 10U < (TEST_FRAMES - 1U) * 9U ||
	    polls > 2U * TEST_FRAMES) {
		test_say(state, "frames: FAIL fences=%u period=%llu us wait median=%llu max=%llu us on-grid=%u/%u one-period=%u registration median=%llu us polls=%u",
		    TEST_FRAMES, (unsigned long long)period_us, (unsigned long long)latencies[TEST_FRAMES / 2U],
		    (unsigned long long)latency_max, aligned, TEST_FRAMES - 1U, single,
		    (unsigned long long)costs[TEST_FRAMES / 2U], polls);
		return;
	}
	test_say(state, "frames: PASS fences=%u period=%llu us wait median=%llu max=%llu us on-grid=%u/%u one-period=%u registration median=%llu us polls=%u",
	    TEST_FRAMES, (unsigned long long)period_us, (unsigned long long)latencies[TEST_FRAMES / 2U],
	    (unsigned long long)latency_max, aligned, TEST_FRAMES - 1U, single,
	    (unsigned long long)costs[TEST_FRAMES / 2U], polls);
}

/* Registers 4 fences together; an all-wait on them and an any-wait on the next group both end at one boundary. */
static void
step_group(
	struct test_state *state,
	struct test_output *output)
{
	VkFence fences[TEST_GROUP];
	VkResult statuses[TEST_GROUP];
	uint64_t started;
	uint64_t all_us;
	uint64_t any_us;
	uint64_t period_us;
	uint32_t index;
	uint32_t signaled;
	VkResult error;

	/* The all-wait. */
	for (index = 0U; index < TEST_GROUP; index++) {
		error = test_register(state, output->display, &fences[index]);
		if (error != VK_SUCCESS) {
			test_say(state, "group: FAIL register error=%d", (int)error);
			return;
		}
	}
	started = test_now_us();
	error = vkWaitForFences(state->device, TEST_GROUP, fences, VK_TRUE, TEST_SECOND_NS);
	all_us = test_now_us() - started;
	for (index = 0U; index < TEST_GROUP; index++)
		vkDestroyFence(state->device, fences[index], NULL);
	if (error != VK_SUCCESS) {
		test_say(state, "group: FAIL all-wait error=%d", (int)error);
		return;
	}

	/* The any-wait, and then every fence of the group signaled. */
	for (index = 0U; index < TEST_GROUP; index++) {
		error = test_register(state, output->display, &fences[index]);
		if (error != VK_SUCCESS) {
			test_say(state, "group: FAIL register error=%d", (int)error);
			return;
		}
	}
	started = test_now_us();
	error = vkWaitForFences(state->device, TEST_GROUP, fences, VK_FALSE, TEST_SECOND_NS);
	any_us = test_now_us() - started;
	test_sleep_ms(5U);
	signaled = 0U;
	for (index = 0U; index < TEST_GROUP; index++) {
		statuses[index] = vkGetFenceStatus(state->device, fences[index]);
		if (statuses[index] == VK_SUCCESS)
			signaled++;
		vkDestroyFence(state->device, fences[index], NULL);
	}

	/* Both waits end within two periods, and the boundary that ended the any-wait signaled all four. */
	period_us = 1000000000U / (uint64_t)output->refresh_mhz;
	if (error != VK_SUCCESS ||
	    all_us > 2U * period_us + 2000U ||
	    any_us > 2U * period_us + 2000U ||
	    signaled != TEST_GROUP) {
		test_say(state, "group: FAIL any-wait error=%d all=%llu us any=%llu us signaled=%u", (int)error,
		    (unsigned long long)all_us, (unsigned long long)any_us, signaled);
		return;
	}
	test_say(state, "group: PASS all=%llu us any=%llu us signaled=%u", (unsigned long long)all_us,
	    (unsigned long long)any_us, signaled);
}

/* A signaled fence reset stays unsignaled at the boundaries after it. */
static void
step_reset(
	struct test_state *state,
	struct test_output *output)
{
	VkFence fence;
	VkResult signaled;
	VkResult after;
	VkResult error;

	/* The boundary signals the fence, the reset spends it, and later boundaries leave it unsignaled. */
	error = test_register(state, output->display, &fence);
	if (error != VK_SUCCESS) {
		test_say(state, "reset: FAIL register error=%d", (int)error);
		return;
	}
	signaled = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, TEST_SECOND_NS);
	vkResetFences(state->device, 1U, &fence);
	test_sleep_ms(200U);
	after = vkGetFenceStatus(state->device, fence);
	vkDestroyFence(state->device, fence, NULL);
	if (signaled != VK_SUCCESS || after != VK_NOT_READY) {
		test_say(state, "reset: FAIL signaled=%d after-reset=%d", (int)signaled, (int)after);
		return;
	}
	test_say(state, "reset: PASS signaled=%d after-reset=%d", (int)signaled, (int)after);
}

/* Fences destroyed before their boundary; afterwards a new one still signals. */
static void
step_cancel(
	struct test_state *state,
	struct test_output *output)
{
	VkFence fences[TEST_CANCELS];
	VkFence fence;
	uint32_t index;
	VkResult error;

	/* Registered together and destroyed at once: the kernel withdraws each. */
	for (index = 0U; index < TEST_CANCELS; index++) {
		error = test_register(state, output->display, &fences[index]);
		if (error != VK_SUCCESS) {
			test_say(state, "cancel: FAIL register %u error=%d", index, (int)error);
			return;
		}
		vkDestroyFence(state->device, fences[index], NULL);
	}

	/* The open's registrations are free again: one more signals. */
	error = test_register(state, output->display, &fence);
	if (error != VK_SUCCESS) {
		test_say(state, "cancel: FAIL register after error=%d", (int)error);
		return;
	}
	error = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, TEST_SECOND_NS);
	vkDestroyFence(state->device, fence, NULL);
	if (error != VK_SUCCESS) {
		test_say(state, "cancel: FAIL wait after error=%d", (int)error);
		return;
	}
	test_say(state, "cancel: PASS destroyed=%u then one signaled", TEST_CANCELS);
}

/* A fence of a display that shows nothing does not signal. */
static void
step_noscan(
	struct test_state *state,
	VkDisplayKHR display)
{
	VkFence fence;
	VkResult error;

	/* Display 1 has no swapchain, so it makes no boundary. */
	error = test_register(state, display, &fence);
	if (error != VK_SUCCESS) {
		test_say(state, "noscan: FAIL register error=%d", (int)error);
		return;
	}
	error = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, TEST_SECOND_NS / 2U);
	vkDestroyFence(state->device, fence, NULL);
	if (error != VK_TIMEOUT) {
		test_say(state, "noscan: FAIL wait=%d (expected a timeout)", (int)error);
		return;
	}
	test_say(state, "noscan: PASS wait=%d", (int)error);
}

/* A fence of display 1 waited for while the host unplugs head 1 returns. */
static void
step_unplug(
	struct test_state *state,
	VkDisplayKHR display)
{
	VkFence fence;
	uint64_t started;
	uint64_t waited_ms;
	VkResult error;

	/* The host unplugs head 1 once it sees the marker; the wait must not run to its end. */
	error = test_register(state, display, &fence);
	if (error != VK_SUCCESS) {
		test_say(state, "unplug: FAIL register error=%d", (int)error);
		return;
	}
	test_marker(state, "unplug");
	started = test_now_us();
	error = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, 60U * TEST_SECOND_NS);
	waited_ms = (test_now_us() - started) / 1000U;
	vkDestroyFence(state->device, fence, NULL);
	if (error != VK_SUCCESS) {
		test_say(state, "unplug: FAIL wait=%d after %llu ms", (int)error, (unsigned long long)waited_ms);
		return;
	}
	test_say(state, "unplug: PASS wait=%d after %llu ms", (int)error, (unsigned long long)waited_ms);
}
