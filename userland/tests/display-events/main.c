/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display events probe (ws113-p003): an independent Vulkan client of
 * VK_EXT_display_control, run with no compositor.  It enumerates the
 * displays, presents a solid colour to each of up to two of them through a
 * swapchain of its own, waits for each one's next refresh boundary, powers
 * the first off and on again, checks that a signaled event fence is not
 * signaled again after a reset, and then watches the hotplug fence for a
 * while: on each signal it registers a new fence, enumerates the displays
 * again and destroys the old fence.  The lines the tests read:
 *
 *   DISPLAY-EVENTS extensions surface-counter=0|1 display-control=0|1
 *   DISPLAY-EVENTS displays count=N
 *   DISPLAY-EVENTS display index=I name=NAME width=W height=H
 *   DISPLAY-EVENTS swapchain index=I result=R        (R is the VkResult; -3 is the output limit)
 *   DISPLAY-EVENTS present index=I frames=F result=R
 *   DISPLAY-EVENTS first-pixel index=I result=R ms=M
 *   DISPLAY-EVENTS showing index=0 seconds=S           (the frames stay shown for the hold)
 *   DISPLAY-EVENTS power index=I state=off|on result=R
 *   DISPLAY-EVENTS reset-spent signaled=R after-reset=R
 *   DISPLAY-EVENTS hotplug count=N names=NAME,NAME
 *   DISPLAY-EVENTS done error=E
 *
 *   display-events [--watch=SECONDS] [--frames=N] [--hold=SECONDS]
 *       watch: how long the hotplug fence is watched (default 10)
 *       frames: frames presented to each display (default 60)
 *       hold: how long the frames stay shown, and how long the first display stays off (default 2)
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* At most this many displays get a swapchain. */
#define EVENTS_OUTPUTS		2U

/* The defaults of the command-line options. */
#define EVENTS_WATCH		10U
#define EVENTS_FRAMES		60U
#define EVENTS_HOLD		2U

/* At most this many displays are enumerated. */
#define EVENTS_DISPLAYS		16U

/* One second of a Vulkan timeout. */
#define EVENTS_SECOND_NS	UINT64_C(1000000000)

/*
 * One display the probe presents to, with its own surface and swapchain.
 * It lives from its swapchain's creation until the end of the probe.
 */
struct events_output {
	VkDisplayKHR display;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkImage *images;
	uint32_t image_count;
	VkExtent2D extent;
	VkClearColorValue colour;
};

/*
 * The probe's Vulkan objects, created in main and destroyed at its end.
 */
struct events_state {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
	VkCommandBuffer commands;
	VkFence done;
	struct events_output outputs[EVENTS_OUTPUTS];
	uint32_t output_count;
};

static VkResult events_create(struct events_state *state, int *display_control);
static void events_destroy(struct events_state *state);
static uint32_t events_enumerate(struct events_state *state, VkDisplayKHR *displays, int report);
static VkResult events_output_open(struct events_state *state, VkDisplayKHR display, uint32_t plane, struct events_output *output);
static VkResult events_plane(struct events_state *state, VkDisplayKHR display, uint32_t used, uint32_t *plane);
static VkResult events_present(struct events_state *state, struct events_output *output, uint32_t frames);
static VkResult events_first_pixel(struct events_state *state, VkDisplayKHR display, uint64_t *milliseconds);
static VkResult events_power(struct events_state *state, VkDisplayKHR display, VkDisplayPowerStateEXT power_state);
static void events_reset_spent(struct events_state *state, VkDisplayKHR display);
static void events_watch(struct events_state *state, uint32_t seconds);
static uint64_t events_now_ms(void);
static uint32_t events_option(const char *argument, const char *name, uint32_t fallback);

/*
 * Runs the display events probe.
 */
int
main(
	int argc,
	char **argv)
{
	struct events_state state;
	VkDisplayKHR displays[EVENTS_DISPLAYS];
	struct events_output *output;
	uint64_t milliseconds;
	uint32_t watch;
	uint32_t frames;
	uint32_t hold;
	uint32_t count;
	uint32_t index;
	uint32_t plane;
	uint32_t used;
	int display_control;
	int argument;
	VkResult error;

	/* Reads the options. */
	watch = EVENTS_WATCH;
	frames = EVENTS_FRAMES;
	hold = EVENTS_HOLD;
	for (argument = 1; argument < argc; argument++) {
		watch = events_option(argv[argument], "--watch=", watch);
		frames = events_option(argv[argument], "--frames=", frames);
		hold = events_option(argv[argument], "--hold=", hold);
	}

	/* Creates the instance and the device with the display extensions. */
	memset(&state, 0, sizeof(state));
	error = events_create(&state, &display_control);
	if (error != VK_SUCCESS) {
		printf("DISPLAY-EVENTS done error=%d\n", (int)error);
		events_destroy(&state);
		return 1;
	}

	/* Lists the displays as the first enumeration sees them. */
	count = events_enumerate(&state, displays, 1);

	/* Gives each of up to two displays a swapchain and a colour of its own. */
	used = 0U;
	for (index = 0U; index < count && state.output_count < EVENTS_OUTPUTS; index++) {
		/* A display no free plane can drive is left out. */
		error = events_plane(&state, displays[index], used, &plane);
		if (error != VK_SUCCESS) {
			printf("DISPLAY-EVENTS swapchain index=%u result=%d\n", index, (int)error);
			continue;
		}

		/* A swapchain refused for the output limit leaves the display out and the probe going. */
		output = &state.outputs[state.output_count];
		error = events_output_open(&state, displays[index], plane, output);
		printf("DISPLAY-EVENTS swapchain index=%u result=%d\n", index, (int)error);
		fflush(stdout);
		if (error != VK_SUCCESS)
			continue;

		/* The first display is red and the second blue. */
		output->colour.float32[3] = 1.0f;
		if (state.output_count == 0U) {
			output->colour.float32[0] = 0.9f;
		} else {
			output->colour.float32[2] = 0.9f;
		}

		/* The plane belongs to this display now, and the output counts. */
		used |= 1U << plane;
		state.output_count++;
	}

	/* Presents to every display, then waits for each one's next boundary. */
	for (index = 0U; index < state.output_count; index++) {
		/* The frames of this display's swapchain. */
		error = events_present(&state, &state.outputs[index], frames);
		printf("DISPLAY-EVENTS present index=%u frames=%u result=%d\n", index, frames, (int)error);

		/* The display's first refresh boundary after the registration. */
		milliseconds = 0U;
		error = events_first_pixel(&state, state.outputs[index].display, &milliseconds);
		printf("DISPLAY-EVENTS first-pixel index=%u result=%d ms=%llu\n", index, (int)error, (unsigned long long)milliseconds);
		fflush(stdout);
	}

	/* The frames stay shown for the hold, so that a picture of them can be taken (T1-355b). */
	if (state.output_count != 0U) {
		printf("DISPLAY-EVENTS showing index=0 seconds=%u\n", hold);
		fflush(stdout);
		sleep(hold);
	}

	/* Powers the first display off and on again, and checks a reset event fence stays reset. */
	if (state.output_count != 0U && display_control) {
		error = events_power(&state, state.outputs[0].display, VK_DISPLAY_POWER_STATE_OFF_EXT);
		printf("DISPLAY-EVENTS power index=0 state=off result=%d\n", (int)error);
		fflush(stdout);
		sleep(hold);
		error = events_power(&state, state.outputs[0].display, VK_DISPLAY_POWER_STATE_ON_EXT);
		printf("DISPLAY-EVENTS power index=0 state=on result=%d\n", (int)error);
		fflush(stdout);
		error = events_present(&state, &state.outputs[0], 2U);
		events_reset_spent(&state, state.outputs[0].display);
	}

	/* Watches the hotplug fence. */
	if (display_control)
		events_watch(&state, watch);

	/* Reports the end and releases every object. */
	printf("DISPLAY-EVENTS done error=0\n");
	fflush(stdout);
	events_destroy(&state);

	/* Succeeded: every step reported its own result. */
	return 0;
}

/* Creates the instance, picks the first physical device and creates the device. */
static VkResult
events_create(
	struct events_state *state,
	int *display_control)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_create;
	VkDeviceCreateInfo device_create;
	VkDeviceQueueCreateInfo queue_create;
	VkCommandPoolCreateInfo pool_create;
	VkCommandBufferAllocateInfo commands_create;
	VkFenceCreateInfo fence_create;
	VkExtensionProperties extensions[32];
	VkQueueFamilyProperties families[8];
	const char *instance_names[3];
	const char *device_names[2];
	uint32_t instance_count;
	uint32_t device_count;
	uint32_t count;
	uint32_t index;
	float priority;
	int surface_counter;
	int match;
	VkResult error;

	/* Finds the surface counter extension among the instance extensions. */
	surface_counter = 0;
	count = sizeof(extensions) / sizeof(extensions[0]);
	error = vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	for (index = 0U; index < count; index++) {
		/* The extension the display control depends on. */
		match = strcmp(extensions[index].extensionName, VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME);
		if (match == 0)
			surface_counter = 1;
	}

	/* The direct-display instance, with the surface counters when offered. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "display-events";
	application.apiVersion = VK_API_VERSION_1_0;
	instance_names[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	instance_names[1] = VK_KHR_DISPLAY_EXTENSION_NAME;
	instance_names[2] = VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME;
	instance_count = 2U;
	if (surface_counter)
		instance_count = 3U;
	memset(&instance_create, 0, sizeof(instance_create));
	instance_create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_create.pApplicationInfo = &application;
	instance_create.enabledExtensionCount = instance_count;
	instance_create.ppEnabledExtensionNames = instance_names;
	error = vkCreateInstance(&instance_create, NULL, &state->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The first physical device presents. */
	count = 1U;
	error = vkEnumeratePhysicalDevices(state->instance, &count, &state->physical);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Finds the display control extension among the device extensions. */
	*display_control = 0;
	count = sizeof(extensions) / sizeof(extensions[0]);
	error = vkEnumerateDeviceExtensionProperties(state->physical, NULL, &count, extensions);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	for (index = 0U; index < count; index++) {
		/* Display control needs the surface counters of the instance too. */
		match = strcmp(extensions[index].extensionName, VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME);
		if (match == 0 && surface_counter)
			*display_control = 1;
	}

	/* Reports which extensions the probe uses. */
	printf("DISPLAY-EVENTS extensions surface-counter=%d display-control=%d\n", surface_counter, *display_control);
	fflush(stdout);

	/* The first queue family with graphics clears the images. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, families);
	for (index = 0U; index < count; index++) {
		/* A graphics family can clear colour images. */
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U)
			break;
	}

	/* A device without graphics cannot clear the images. */
	if (index == count)
		return VK_ERROR_INITIALIZATION_FAILED;
	state->family = index;

	/* The device with the swapchain, and the display control when offered. */
	priority = 1.0f;
	memset(&queue_create, 0, sizeof(queue_create));
	queue_create.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_create.queueFamilyIndex = state->family;
	queue_create.queueCount = 1U;
	queue_create.pQueuePriorities = &priority;
	device_names[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	device_names[1] = VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME;
	device_count = 1U;
	if (*display_control)
		device_count = 2U;
	memset(&device_create, 0, sizeof(device_create));
	device_create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_create.queueCreateInfoCount = 1U;
	device_create.pQueueCreateInfos = &queue_create;
	device_create.enabledExtensionCount = device_count;
	device_create.ppEnabledExtensionNames = device_names;
	error = vkCreateDevice(state->physical, &device_create, NULL, &state->device);
	if (error != VK_SUCCESS)
		return error;
	vkGetDeviceQueue(state->device, state->family, 0U, &state->queue);

	/* One command buffer records every clear; one fence waits for each. */
	memset(&pool_create, 0, sizeof(pool_create));
	pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_create.queueFamilyIndex = state->family;
	error = vkCreateCommandPool(state->device, &pool_create, NULL, &state->pool);
	if (error != VK_SUCCESS)
		return error;
	memset(&commands_create, 0, sizeof(commands_create));
	commands_create.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commands_create.commandPool = state->pool;
	commands_create.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commands_create.commandBufferCount = 1U;
	error = vkAllocateCommandBuffers(state->device, &commands_create, &state->commands);
	if (error != VK_SUCCESS)
		return error;
	memset(&fence_create, 0, sizeof(fence_create));
	fence_create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	error = vkCreateFence(state->device, &fence_create, NULL, &state->done);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the device can present and clear. */
	return VK_SUCCESS;
}

/* Destroys the swapchains, surfaces, device and instance that exist. */
static void
events_destroy(
	struct events_state *state)
{
	uint32_t index;

	/* Waits for the queue before its objects go. */
	if (state->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(state->device);

	/* Each output's swapchain goes before its surface. */
	for (index = 0U; index < state->output_count; index++) {
		vkDestroySwapchainKHR(state->device, state->outputs[index].swapchain, NULL);
		vkDestroySurfaceKHR(state->instance, state->outputs[index].surface, NULL);
		free(state->outputs[index].images);
	}

	/* The device's objects, then the device and the instance. */
	if (state->device != VK_NULL_HANDLE) {
		vkDestroyFence(state->device, state->done, NULL);
		vkDestroyCommandPool(state->device, state->pool, NULL);
		vkDestroyDevice(state->device, NULL);
	}

	/* The instance goes last. */
	if (state->instance != VK_NULL_HANDLE)
		vkDestroyInstance(state->instance, NULL);
}

/* Enumerates the displays, reporting each when asked, and returns how many there are. */
static uint32_t
events_enumerate(
	struct events_state *state,
	VkDisplayKHR *displays,
	int report)
{
	VkDisplayPropertiesKHR properties[EVENTS_DISPLAYS];
	const char *name;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* The displays connected now, up to the probe's limit. */
	count = EVENTS_DISPLAYS;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR(state->physical, &count, properties);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		count = 0U;

	/* Keeps each handle, and reports each display on the first enumeration. */
	if (report)
		printf("DISPLAY-EVENTS displays count=%u\n", count);
	for (index = 0U; index < count; index++) {
		/* The handle stays the display's for the instance's life. */
		displays[index] = properties[index].display;

		/* A display without a name prints as a dash. */
		name = "-";
		if (properties[index].displayName != NULL)
			name = properties[index].displayName;

		/* The first enumeration reports the display. */
		if (report) {
			printf("DISPLAY-EVENTS display index=%u name=%s width=%u height=%u\n",
			       index,
			       name,
			       properties[index].physicalResolution.width,
			       properties[index].physicalResolution.height);
		}
	}

	/* The report reaches the log before the next step. */
	fflush(stdout);

	/* Succeeded: the number of displays now. */
	return count;
}

/* Finds a plane not used yet that can drive the display. */
static VkResult
events_plane(
	struct events_state *state,
	VkDisplayKHR display,
	uint32_t used,
	uint32_t *plane)
{
	VkDisplayKHR supported[EVENTS_DISPLAYS];
	uint32_t count;
	uint32_t plane_count;
	uint32_t index;
	uint32_t at;
	VkResult error;

	/* The number of planes of every display together. */
	plane_count = 0U;
	error = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(state->physical, &plane_count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* The first plane not used yet whose displays include this one. */
	for (index = 0U; index < plane_count && index < 32U; index++) {
		/* A plane already given to another display is skipped. */
		if ((used & (1U << index)) != 0U)
			continue;

		/* The displays this plane can drive. */
		count = EVENTS_DISPLAYS;
		error = vkGetDisplayPlaneSupportedDisplaysKHR(state->physical, index, &count, supported);
		if (error != VK_SUCCESS && error != VK_INCOMPLETE)
			return error;
		for (at = 0U; at < count; at++) {
			/* The plane drives this display. */
			if (supported[at] == display) {
				*plane = index;
				return VK_SUCCESS;
			}
		}
	}

	/* No free plane drives the display. */
	return VK_ERROR_INITIALIZATION_FAILED;
}

/* Creates a surface on the display's first mode and a swapchain on it. */
static VkResult
events_output_open(
	struct events_state *state,
	VkDisplayKHR display,
	uint32_t plane,
	struct events_output *output)
{
	VkDisplayModePropertiesKHR modes[8];
	VkDisplaySurfaceCreateInfoKHR surface_create;
	VkSwapchainCreateInfoKHR swapchain_create;
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[8];
	uint32_t count;
	VkResult error;

	/* The display's first mode decides the extent. */
	memset(output, 0, sizeof(*output));
	output->display = display;
	count = sizeof(modes) / sizeof(modes[0]);
	error = vkGetDisplayModePropertiesKHR(state->physical, display, &count, modes);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	output->extent = modes[0].parameters.visibleRegion;

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

	/* The surface's limits and its first format. */
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(state->physical, output->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;
	count = sizeof(formats) / sizeof(formats[0]);
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(state->physical, output->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A FIFO swapchain whose images are cleared by transfers. */
	memset(&swapchain_create, 0, sizeof(swapchain_create));
	swapchain_create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_create.surface = output->surface;
	swapchain_create.minImageCount = capabilities.minImageCount;
	swapchain_create.imageFormat = formats[0].format;
	swapchain_create.imageColorSpace = formats[0].colorSpace;
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
		vkDestroySurfaceKHR(state->instance, output->surface, NULL);
		output->surface = VK_NULL_HANDLE;
		return error;
	}

	/* The swapchain's images. */
	error = vkGetSwapchainImagesKHR(state->device, output->swapchain, &output->image_count, NULL);
	if (error != VK_SUCCESS)
		return error;
	output->images = calloc(output->image_count, sizeof(*output->images));
	if (output->images == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	error = vkGetSwapchainImagesKHR(state->device, output->swapchain, &output->image_count, output->images);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the display has a swapchain of its own. */
	return VK_SUCCESS;
}

/* Presents frames of the output's solid colour. */
static VkResult
events_present(
	struct events_state *state,
	struct events_output *output,
	uint32_t frames)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkImageSubresourceRange range;
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	uint32_t frame;
	uint32_t image;
	VkResult error;

	/* The whole colour image. */
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1U;
	range.layerCount = 1U;

	/* Each frame: acquire, clear, present. */
	for (frame = 0U; frame < frames; frame++) {
		/* The next image, with the fence saying it is free. */
		error = vkAcquireNextImageKHR(state->device, output->swapchain, EVENTS_SECOND_NS, VK_NULL_HANDLE, state->done, &image);
		if (error != VK_SUCCESS)
			return error;
		error = vkWaitForFences(state->device, 1U, &state->done, VK_TRUE, EVENTS_SECOND_NS);
		if (error != VK_SUCCESS)
			return error;
		error = vkResetFences(state->device, 1U, &state->done);
		if (error != VK_SUCCESS)
			return error;

		/* Clears the image to the colour and hands it to presentation. */
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
		error = vkWaitForFences(state->device, 1U, &state->done, VK_TRUE, EVENTS_SECOND_NS);
		if (error != VK_SUCCESS)
			return error;
		error = vkResetFences(state->device, 1U, &state->done);
		if (error != VK_SUCCESS)
			return error;

		/* Presents the cleared image. */
		memset(&present, 0, sizeof(present));
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.swapchainCount = 1U;
		present.pSwapchains = &output->swapchain;
		present.pImageIndices = &image;
		error = vkQueuePresentKHR(state->queue, &present);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every frame was presented. */
	return VK_SUCCESS;
}

/* Waits up to a second for the display's next refresh boundary through an event fence. */
static VkResult
events_first_pixel(
	struct events_state *state,
	VkDisplayKHR display,
	uint64_t *milliseconds)
{
	PFN_vkRegisterDisplayEventEXT register_event;
	VkDisplayEventInfoEXT event;
	VkFence fence;
	uint64_t started;
	VkResult error;

	/* The entry point is reached through the device, as the extension is enabled there. */
	register_event = (PFN_vkRegisterDisplayEventEXT)vkGetDeviceProcAddr(state->device, "vkRegisterDisplayEventEXT");
	if (register_event == NULL)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Registers the event and times the wait for it. */
	memset(&event, 0, sizeof(event));
	event.sType = VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT;
	event.displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT;
	error = register_event(state->device, display, &event, NULL, &fence);
	if (error != VK_SUCCESS)
		return error;
	started = events_now_ms();
	error = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, EVENTS_SECOND_NS);
	*milliseconds = events_now_ms() - started;
	vkDestroyFence(state->device, fence, NULL);

	/* Reports the wait's result: success, or a timeout when no boundary came. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: a boundary came after the registration. */
	return VK_SUCCESS;
}

/* Asks the display into a power state. */
static VkResult
events_power(
	struct events_state *state,
	VkDisplayKHR display,
	VkDisplayPowerStateEXT power_state)
{
	PFN_vkDisplayPowerControlEXT power_control;
	VkDisplayPowerInfoEXT power;
	VkResult error;

	/* The entry point is reached through the device. */
	power_control = (PFN_vkDisplayPowerControlEXT)vkGetDeviceProcAddr(state->device, "vkDisplayPowerControlEXT");
	if (power_control == NULL)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* The requested state. */
	memset(&power, 0, sizeof(power));
	power.sType = VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT;
	power.powerState = power_state;
	error = power_control(state->device, display, &power);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the display took the state. */
	return VK_SUCCESS;
}

/* Checks that a signaled display event fence is not signaled again after a reset. */
static void
events_reset_spent(
	struct events_state *state,
	VkDisplayKHR display)
{
	PFN_vkRegisterDisplayEventEXT register_event;
	VkDisplayEventInfoEXT event;
	VkFence fence;
	VkResult signaled;
	VkResult after_reset;

	/* The entry point is reached through the device. */
	register_event = (PFN_vkRegisterDisplayEventEXT)vkGetDeviceProcAddr(state->device, "vkRegisterDisplayEventEXT");
	if (register_event == NULL)
		return;

	/* Waits for the event, resets the fence, and looks again after several boundaries. */
	memset(&event, 0, sizeof(event));
	event.sType = VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT;
	event.displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT;
	signaled = register_event(state->device, display, &event, NULL, &fence);
	if (signaled != VK_SUCCESS) {
		printf("DISPLAY-EVENTS reset-spent signaled=%d after-reset=0\n", (int)signaled);
		return;
	}

	/* The boundary signals the fence; the reset must keep it unsignaled at later boundaries. */
	signaled = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, EVENTS_SECOND_NS);
	vkResetFences(state->device, 1U, &fence);
	usleep(200000);
	after_reset = vkGetFenceStatus(state->device, fence);
	vkDestroyFence(state->device, fence, NULL);

	/* Signaled is 0 (success) and after-reset 1 (not ready) when the event is not replayed. */
	printf("DISPLAY-EVENTS reset-spent signaled=%d after-reset=%d\n", (int)signaled, (int)after_reset);
	fflush(stdout);
}

/* Watches the hotplug fence, enumerating the displays again on each signal. */
static void
events_watch(
	struct events_state *state,
	uint32_t seconds)
{
	PFN_vkRegisterDeviceEventEXT register_event;
	VkDeviceEventInfoEXT event;
	VkDisplayPropertiesKHR properties[EVENTS_DISPLAYS];
	VkFence fence;
	VkFence next;
	const char *name;
	uint64_t deadline;
	uint64_t now;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* The entry point is reached through the device. */
	register_event = (PFN_vkRegisterDeviceEventEXT)vkGetDeviceProcAddr(state->device, "vkRegisterDeviceEventEXT");
	if (register_event == NULL)
		return;

	/* The first hotplug fence. */
	memset(&event, 0, sizeof(event));
	event.sType = VK_STRUCTURE_TYPE_DEVICE_EVENT_INFO_EXT;
	event.deviceEvent = VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT;
	error = register_event(state->device, &event, NULL, &fence);
	if (error != VK_SUCCESS) {
		printf("DISPLAY-EVENTS hotplug register result=%d\n", (int)error);
		return;
	}

	/* Reports the start of the watch. */
	printf("DISPLAY-EVENTS hotplug watching seconds=%u\n", seconds);
	fflush(stdout);

	/* Until the deadline, each signal is followed by a new fence, a new enumeration and the old fence's end. */
	deadline = events_now_ms() + (uint64_t)seconds * 1000U;
	for (;;) {
		/* The watch ends at its deadline. */
		now = events_now_ms();
		if (now >= deadline)
			break;

		/* Waits up to a quarter second for the next signal. */
		error = vkWaitForFences(state->device, 1U, &fence, VK_TRUE, EVENTS_SECOND_NS / 4U);
		if (error != VK_SUCCESS)
			continue;

		/* A new fence first, so a change during the enumeration is not missed. */
		error = register_event(state->device, &event, NULL, &next);
		if (error != VK_SUCCESS)
			break;
		vkDestroyFence(state->device, fence, NULL);
		fence = next;

		/* The displays as they are now. */
		count = EVENTS_DISPLAYS;
		error = vkGetPhysicalDeviceDisplayPropertiesKHR(state->physical, &count, properties);
		if (error != VK_SUCCESS && error != VK_INCOMPLETE)
			count = 0U;

		/* Reports the displays, their names separated by commas. */
		printf("DISPLAY-EVENTS hotplug count=%u names=", count);
		for (index = 0U; index < count; index++) {
			/* A comma before every name but the first. */
			if (index != 0U)
				printf(",");

			/* A display without a name prints as a dash. */
			name = "-";
			if (properties[index].displayName != NULL)
				name = properties[index].displayName;
			printf("%s", name);
		}

		/* The line reaches the log at once. */
		printf("\n");
		fflush(stdout);
	}

	/* The last fence goes with the watch. */
	vkDestroyFence(state->device, fence, NULL);
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
events_now_ms(void)
{
	struct timespec now;
	int error;

	/* The monotonic clock; a failure reads as zero. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: the time in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Reads one numeric option, keeping the fallback when the argument is another one. */
static uint32_t
events_option(
	const char *argument,
	const char *name,
	uint32_t fallback)
{
	size_t length;
	int match;

	/* Another option leaves the value as it was. */
	length = strlen(name);
	match = strncmp(argument, name, length);
	if (match != 0)
		return fallback;

	/* Succeeded: the option's value. */
	return (uint32_t)strtoul(argument + length, NULL, 10);
}
