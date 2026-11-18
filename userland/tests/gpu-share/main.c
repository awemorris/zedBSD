/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Verifies a real Venus allocation after its producer process has exited.
 * The private WSI allocation helpers are linked directly into this test only.
 * Readback is an independent pixel oracle, never the WSI presentation path.
 */

#include "wsi-internal.h"
#include <uapi/gpu-display.h>
#include <uapi/gpu-allocation.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SHARE_EDGE 32U
#define SHARE_BYTES (SHARE_EDGE * SHARE_EDGE * 4U)
#define SHARE_TIMEOUT 10000000000ULL
#define SHARE_EXTERNAL_FAMILY 0xfffffffeU
#define SHARE_LINEAR 0U
#define SHARE_BUFFER 1U
#define SHARE_OPTIMAL 2U

/*
 * One process owns all of these Vulkan objects and an independent renderer context.
 * The producer exports only its image allocation; no device or queue is inherited.
 */
struct share_context {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkImage image;
	VkBuffer shared_buffer;
	VkDeviceMemory image_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	struct gpu_image_descriptor descriptor;
	const char *operation;
};

/* The selected CLI scenario is fixed before fork and shared as a value, never as a Vulkan context. */
static unsigned share_kind;

static int share_displays(void);
static VkResult share_open(struct share_context *context);
static VkResult share_close(struct share_context *context);
static VkResult share_begin(struct share_context *context);
static VkResult share_submit(struct share_context *context);
static VkResult share_produce(struct share_context *context, int *descriptor);
static VkResult share_receive(struct share_context *context, int *descriptor);
static VkResult share_buffer(struct share_context *context);
static VkResult share_verify(struct share_context *context);
static void share_barrier(struct share_context *context, VkImageLayout old_layout, VkImageLayout new_layout, uint32_t source_family, uint32_t destination_family, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static int share_send_fd(int socket, int descriptor);
static int share_receive_fd(int socket, int *descriptor);
static int share_producer(int socket);
static VkResult share_allocation_object(struct share_context *context, VkMemoryRequirements *requirements);
static VkResult share_allocation_create(struct share_context *context, int *fd);
static VkResult share_allocation_import(struct share_context *context, int *fd);

/*
 * Imports and checks pixels only after observing the producing process's exit.
 */
int
main(
	int argc,
	char **argv)
{
	struct share_context context;
	VkResult result;
	VkResult cleanup;
	pid_t producer;
	pid_t reaped;
	int sockets[2];
	int descriptor;
	int child_status;
	int error;
	int selected;

	/* The default retains the original linear-image regression; explicit modes test raw allocations. */
	if (argc == 2) {
		/* A buffer capability has no image interpretation or native scanout requirement. */
		selected = strcmp(argv[1], "--buffer");
		if (selected == 0) {
			share_kind = SHARE_BUFFER;
		} else {
			/* The optimal-image case verifies native tiling through a separate renderer import. */
			selected = strcmp(argv[1], "--optimal");
			if (selected != 0) {
				fprintf(stderr, "usage: gpu-share-test [--buffer|--optimal]\n");
				return 2;
			}

			/* Both processes use the same immutable test schema selected before any GPU open. */
			share_kind = SHARE_OPTIMAL;
		}
	} else if (argc != 1) {
		fprintf(stderr, "usage: gpu-share-test [--buffer|--optimal]\n");
		return 2;
	}

	/* Native discovery reports actual QEMU timing metadata before either process creates Vulkan objects. */
	error = share_displays();
	if (error != 0) {
		perror("gpu-share-client: native display inventory");
		return 1;
	}

	/* Fork occurs before any Vulkan instance or renderer context exists. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	if (error != 0) {
		perror("gpu-share-client: socketpair");
		return 1;
	}

	/* The child exclusively owns producer-side Vulkan objects. */
	producer = fork();
	if (producer < 0) {
		perror("gpu-share-client: fork");
		close(sockets[0]);
		close(sockets[1]);
		return 1;
	}

	/* Child exit follows explicit Vulkan cleanup and closure of its export fd. */
	if (producer == 0) {
		close(sockets[0]);
		error = share_producer(sockets[1]);
		close(sockets[1]);
		_exit(error);
	}

	/* SCM_RIGHTS is the only allocation authority carried between the processes. */
	close(sockets[1]);
	descriptor = -1;
	error = share_receive_fd(sockets[0], &descriptor);
	close(sockets[0]);

	/* A successful wait proves import happens after producer process teardown. */
	reaped = waitpid(producer, &child_status, 0);
	if (reaped != producer || error != 0) {
		fprintf(stderr, "GPU SHARE FAILED receive=%d wait=%d errno=%d\n", error, (int)reaped, errno);
		if (descriptor >= 0)
			close(descriptor);
		return 1;
	}

	/* A producer-side API or teardown failure is never replaced by a pixel comparison. */
	if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
		fprintf(stderr, "GPU SHARE FAILED producer status=%d\n", child_status);
		close(descriptor);
		return 1;
	}

	/* This fresh Vulkan instance creates an independent host renderer context. */
	memset(&context, 0, sizeof(context));
	result = share_open(&context);
	if (result == VK_SUCCESS)
		result = share_receive(&context, &descriptor);

	/* A failed import still leaves the input capability owned by this caller. */
	if (descriptor >= 0)
		close(descriptor);
	if (result != VK_SUCCESS)
		fprintf(stderr, "GPU SHARE FAILED receiver api=%s result=%d\n", context.operation, (int)result);

	/* Cleanup participates in the result even after the pixel oracle succeeds. */
	cleanup = share_close(&context);
	if (cleanup != VK_SUCCESS) {
		fprintf(stderr, "GPU SHARE FAILED receiver cleanup=%d\n", (int)cleanup);
		return 1;
	}

	/* An actual renderer or content failure prevents the terminal success record. */
	if (result != VK_SUCCESS)
		return 1;

	/* Succeeded: a new process used and released the producer's actual GPU allocation. */
	puts("GPU SHARE PASS producer-exit SCM_RIGHTS independent-context import GPU-copy pixels=1024 rgba=ff00ffff final-release");
	printf("GPU ALLOCATION PASS kind=%u producer-exit independent-context pixels=1024\n", share_kind);
	return 0;
}

/* Reports native output and mode metadata through the actual GPU ioctls without selecting scanout. */
static int
share_displays(void)
{
	struct gpu_display_info output;
	struct gpu_display_mode mode;
	uint32_t outputs;
	uint32_t modes;
	uint32_t index;
	uint32_t ordinal;
	int descriptor;
	int error;
	int saved;

	/* This diagnostic opens and closes its own context before either Vulkan process exists. */
	descriptor = open("/dev/gpu0", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;

	/* The count query has no synthetic framebuffer or hardcoded resolution source. */
	memset(&output, 0, sizeof(output));
	output.version = GPU_ABI_VERSION;
	output.size = sizeof(output);
	output.index = GPU_DISPLAY_COUNT_ONLY;
	error = ioctl(descriptor, GPU_DISPLAY_QUERY, &output);
	if (error != 0)
		goto cleanup;

	/* Virtio's protocol supports at most sixteen native scanouts. */
	outputs = output.count;
	if (outputs > 16U) {
		errno = EOVERFLOW;
		error = -1;
		goto cleanup;
	}

	/* Enumerate each actual connected output without changing its lease or selected image. */
	for (index = 0U; index < outputs; index++) {
		memset(&output, 0, sizeof(output));
		output.version = GPU_ABI_VERSION;
		output.size = sizeof(output);
		output.index = index;
		error = ioctl(descriptor, GPU_DISPLAY_QUERY, &output);
		if (error != 0)
			goto cleanup;
		printf("GPU DISPLAY index=%u id=%u connected=%u preferred=%ux%u refresh_millihz=%u physical_mm=%ux%u flags=%u\n", index, output.display_id, output.flags & GPU_DISPLAY_CONNECTED, output.preferred_width, output.preferred_height, output.refresh_millihz, output.physical_width_mm, output.physical_height_mm, output.flags);

		/* Disconnected outputs have no mode-validation or ownership contract to query. */
		if ((output.flags & GPU_DISPLAY_CONNECTED) == 0U)
			continue;

		/* Native mode count is obtained from the same observed topology generation. */
		memset(&mode, 0, sizeof(mode));
		mode.version = GPU_ABI_VERSION;
		mode.size = sizeof(mode);
		mode.display_id = output.display_id;
		mode.generation = output.generation;
		mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
		mode.index = GPU_DISPLAY_COUNT_ONLY;
		error = ioctl(descriptor, GPU_DISPLAY_MODE, &mode);
		if (error != 0)
			goto cleanup;

		/* Eight EDID blocks contain fewer than sixty-four complete detailed timings. */
		modes = mode.count;
		if (modes == 0U || modes > 64U) {
			errno = EOVERFLOW;
			error = -1;
			goto cleanup;
		}

		/* Print the kernel's actual dimensions and nominal frequencies for the capture record. */
		for (ordinal = 0U; ordinal < modes; ordinal++) {
			memset(&mode, 0, sizeof(mode));
			mode.version = GPU_ABI_VERSION;
			mode.size = sizeof(mode);
			mode.display_id = output.display_id;
			mode.generation = output.generation;
			mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
			mode.index = ordinal;
			error = ioctl(descriptor, GPU_DISPLAY_MODE, &mode);
			if (error != 0)
				goto cleanup;
			printf("GPU MODE display=%u index=%u width=%u height=%u refresh_millihz=%u\n", output.display_id, ordinal, mode.width, mode.height, mode.refresh_millihz);
		}
	}

cleanup:
	/* No diagnostic GPU context may be inherited by the later independent-process renderer check. */
	saved = errno;
	close(descriptor);
	fflush(stdout);
	errno = saved;
	if (error != 0)
		return -1;

	/* Succeeded: the record describes actual native discovery without acquiring a display lease. */
	return 0;
}

/* Creates one independent device, command buffer and bounded submission fence. */
static VkResult
share_open(
	struct share_context *context)
{
	const char *instance_extensions[2];
	const char *device_extensions[2];
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkQueueFamilyProperties families[32];
	VkPhysicalDevice devices[8];
	VkResult result;
	float priority;
	uint32_t count;
	uint32_t index;

	/* No window system or display lease is involved in the renderer import oracle. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "gpu-share-client";
	application.apiVersion = VK_API_VERSION_1_0;

	/* Instance creation opens a GPU context owned solely by this process. */
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	if (share_kind != SHARE_LINEAR) {
		instance_extensions[0] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
		instance_extensions[1] = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
		instance.enabledExtensionCount = 2U;
		instance.ppEnabledExtensionNames = instance_extensions;
	}
	context->operation = "vkCreateInstance";
	result = vkCreateInstance(&instance, NULL, &context->instance);
	if (result != VK_SUCCESS)
		return result;

	/* The bounded inventory rejects truncation instead of silently changing devices. */
	count = 8U;
	context->operation = "vkEnumeratePhysicalDevices";
	result = vkEnumeratePhysicalDevices(context->instance, &count, devices);
	if (result != VK_SUCCESS)
		return result;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	context->physical = devices[0];

	/* Pick an actual graphics family rather than assuming queue family zero. */
	count = 32U;
	vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &count, families);
	context->family = UINT32_MAX;
	for (index = 0U; index < count; index++) {
		/* A graphics queue provides the transfer commands exercised by this fixture. */
		if (families[index].queueCount != 0U &&
		    (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
			context->family = index;
			break;
		}
	}

	/* Unsupported queue inventories fail before a logical device is created. */
	if (context->family == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* One queue suffices to establish explicit external ownership transitions. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = context->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;

	/* Standard buffer and optimal-image scenarios explicitly enable the fd extension. */
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1U;
	device.pQueueCreateInfos = &queue;
	if (share_kind != SHARE_LINEAR) {
		device_extensions[0] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
		device_extensions[1] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
		device.enabledExtensionCount = 2U;
		device.ppEnabledExtensionNames = device_extensions;
	}
	context->operation = "vkCreateDevice";
	result = vkCreateDevice(context->physical, &device, NULL, &context->device);
	if (result != VK_SUCCESS)
		return result;

	/* The returned queue belongs to this newly created logical device. */
	vkGetDeviceQueue(context->device, context->family, 0U, &context->queue);

	/* All fixture commands use the selected family and one primary buffer. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.queueFamilyIndex = context->family;
	context->operation = "vkCreateCommandPool";
	result = vkCreateCommandPool(context->device, &pool, NULL, &context->pool);
	if (result != VK_SUCCESS)
		return result;

	/* A pool owns its command buffer through normal Vulkan teardown. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = context->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	context->operation = "vkAllocateCommandBuffers";
	result = vkAllocateCommandBuffers(context->device, &command, &context->command);
	if (result != VK_SUCCESS)
		return result;

	/* Every submitted command is observed with a finite GPU timeout. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	context->operation = "vkCreateFence";
	result = vkCreateFence(context->device, &fence, NULL, &context->fence);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: this process has its own renderer command namespace. */
	return VK_SUCCESS;
}

/* Releases all Vulkan ownership, including the imported allocation's kernel alias. */
static VkResult
share_close(
	struct share_context *context)
{
	VkResult result;

	/* A partially initialized context still releases every successfully created object. */
	result = VK_SUCCESS;
	if (context->device != VK_NULL_HANDLE) {
		/* Observe device completion before retiring command and memory ownership. */
		result = vkDeviceWaitIdle(context->device);

		/* Command buffers stop referring to the image before its destruction. */
		if (context->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(context->device, context->pool, NULL);

		/* The completion fence owns no commands after device idle. */
		if (context->fence != VK_NULL_HANDLE)
			vkDestroyFence(context->device, context->fence, NULL);

		/* Resource objects retire before the allocations to which they were bound. */
		if (context->readback != VK_NULL_HANDLE)
			vkDestroyBuffer(context->device, context->readback, NULL);
		if (context->readback_memory != VK_NULL_HANDLE)
			vkFreeMemory(context->device, context->readback_memory, NULL);
		if (context->image != VK_NULL_HANDLE)
			vkDestroyImage(context->device, context->image, NULL);

		/* Buffer scenarios bind the same allocation ownership without creating an image. */
		if (context->shared_buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(context->device, context->shared_buffer, NULL);

		/* The underlying allocation outlives whichever type of resource was bound to it. */
		if (context->image_memory != VK_NULL_HANDLE)
			vkFreeMemory(context->device, context->image_memory, NULL);

		/* Destroying the device cannot be substituted by retaining the entire producer session. */
		vkDestroyDevice(context->device, NULL);
	}

	/* Instance teardown closes the final process-owned GPU session. */
	if (context->instance != VK_NULL_HANDLE)
		vkDestroyInstance(context->instance, NULL);

	/* A failed GPU completion is preserved even though local ownership was consumed. */
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: this process owns no remaining Vulkan resource or context. */
	return VK_SUCCESS;
}

/* Begins the single one-time command buffer used by either process. */
static VkResult
share_begin(
	struct share_context *context)
{
	VkCommandBufferBeginInfo begin;
	VkResult result;

	/* Each independent process records only one submission in its fresh command buffer. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	context->operation = "vkBeginCommandBuffer";
	result = vkBeginCommandBuffer(context->command, &begin);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: image ownership and transfer commands may be recorded. */
	return VK_SUCCESS;
}

/* Submits the recorded GPU work and requires its actual completion fence. */
static VkResult
share_submit(
	struct share_context *context)
{
	VkSubmitInfo submit;
	VkResult result;

	/* Recording failures must be visible before any host queue submission. */
	context->operation = "vkEndCommandBuffer";
	result = vkEndCommandBuffer(context->command);
	if (result != VK_SUCCESS)
		return result;

	/* This fence, rather than socket progress, orders all subsequent allocation use. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &context->command;
	context->operation = "vkQueueSubmit";
	result = vkQueueSubmit(context->queue, 1U, &submit, context->fence);
	if (result != VK_SUCCESS)
		return result;

	/* A bounded timeout reports missing renderer completion as a test failure. */
	context->operation = "vkWaitForFences";
	result = vkWaitForFences(context->device, 1U, &context->fence, VK_TRUE, SHARE_TIMEOUT);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: all image writes or receiver copies completed on the real GPU. */
	return VK_SUCCESS;
}

/* Clears an exportable GPU image and releases its ownership to an external consumer. */
static VkResult
share_produce(
	struct share_context *context,
	int *descriptor)
{
	VkExtent2D extent;
	VkClearColorValue color;
	VkImageSubresourceRange range;
	VkResult result;

	/* Allocation and image creation use the real WSI helper, not a fixture-created host resource. */
	extent.width = SHARE_EDGE;
	extent.height = SHARE_EDGE;
	if (share_kind == SHARE_LINEAR) {
		context->operation = "vulkan_wsi_shared_image_create";
		result = vulkan_wsi_shared_image_create(context->device, VK_FORMAT_R8G8B8A8_UNORM, extent, NULL, NULL, &context->image, &context->image_memory, descriptor, &context->descriptor);
	} else {
		/* The generic capability preserves allocation ownership without a linear display descriptor. */
		result = share_allocation_create(context, descriptor);
	}

	/* No GPU writes are recorded for a failed resource or allocation export. */
	if (result != VK_SUCCESS)
		return result;

	/* The producing GPU command owns and initializes the entire pixel allocation. */
	result = share_begin(context);
	if (result != VK_SUCCESS)
		return result;
	share_barrier(context, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0U, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* Exact endpoint channels allow a byte-exact independent pixel oracle. */
	memset(&color, 0, sizeof(color));
	color.float32[0] = 1.0f;
	color.float32[2] = 1.0f;
	color.float32[3] = 1.0f;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1U;
	range.layerCount = 1U;
	if (share_kind == SHARE_BUFFER) {
		/* The raw byte pattern matches the image oracle on this explicitly amd64 test target. */
		vkCmdFillBuffer(context->command, context->shared_buffer, 0U, SHARE_BYTES, 0xffff00ffU);
	} else {
		/* A renderer image clear proves that optimal storage is interpreted by the actual GPU. */
		vkCmdClearColorImage(context->command, context->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1U, &range);
	}

	/* The next process acquires the same GENERAL-layout external allocation. */
	share_barrier(context, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, context->family, SHARE_EXTERNAL_FAMILY, VK_ACCESS_TRANSFER_WRITE_BIT, 0U, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	result = share_submit(context);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: exporting the fd cannot race the producer's GPU writes. */
	return VK_SUCCESS;
}

/* Imports the exited producer's allocation and checks a real GPU copy of its pixels. */
static VkResult
share_receive(
	struct share_context *context,
	int *descriptor)
{
	VkBufferImageCopy copy;
	VkBufferCopy buffer_copy;
	VkBufferMemoryBarrier visible;
	VkResult result;

	/*
	 * This performs CTX_ATTACH, VkImportMemoryResourceInfoMESA (the record
	 * named in venus-protocol xmls/VK_MESA_venus_protocol.xml), and VkImage
	 * binding.
	 */
	if (share_kind == SHARE_LINEAR) {
		context->operation = "vulkan_wsi_shared_image_import";
		result = vulkan_wsi_shared_image_import(context->device, *descriptor, NULL, &context->image, &context->image_memory, &context->descriptor);
	} else {
		/* The receiver interprets its negotiated metadata after the kernel attaches the raw allocation. */
		result = share_allocation_import(context, descriptor);
	}

	/* Failed imports retain no usable receiver resource. */
	if (result != VK_SUCCESS)
		return result;

	/* The renderer alias must remain usable after the final transferable capability closes. */
	if (*descriptor >= 0)
		close(*descriptor);
	*descriptor = -1;

	/* Authoritative kernel metadata must describe the producer's exact image. */
	context->operation = "imported image metadata";
	if (share_kind == SHARE_LINEAR) {
		/* The image-specific protocol alone carries authoritative scanout geometry. */
		if (context->descriptor.width != SHARE_EDGE ||
		    context->descriptor.height != SHARE_EDGE ||
		    context->descriptor.format != GPU_PIXEL_RGBA8888)
			return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A separate visible buffer is only the final test oracle, not shared image storage. */
	result = share_buffer(context);
	if (result != VK_SUCCESS)
		return result;
	result = share_begin(context);
	if (result != VK_SUCCESS)
		return result;

	/* Import acquires external ownership before any receiver renderer command reads pixels. */
	share_barrier(context, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SHARE_EXTERNAL_FAMILY, context->family, 0U, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* The renderer copies the imported image into a distinct ordinary allocation. */
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1U;
	copy.imageExtent.width = SHARE_EDGE;
	copy.imageExtent.height = SHARE_EDGE;
	copy.imageExtent.depth = 1U;
	if (share_kind == SHARE_BUFFER) {
		/* The import supplies source storage; the test allocates only the separate oracle destination. */
		memset(&buffer_copy, 0, sizeof(buffer_copy));
		buffer_copy.size = SHARE_BYTES;
		vkCmdCopyBuffer(context->command, context->shared_buffer, context->readback, 1U, &buffer_copy);
	} else {
		/* Native image transfer decodes linear or optimal tiling without any guessed CPU row pitch. */
		vkCmdCopyImageToBuffer(context->command, context->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, context->readback, 1U, &copy);
	}

	/* Host visibility follows the completed GPU write rather than coherent-memory assumptions. */
	memset(&visible, 0, sizeof(visible));
	visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	visible.buffer = context->readback;
	visible.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(context->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U, &visible, 0U, NULL);

	/* Complete receiver use returns the allocation to its external ownership domain. */
	share_barrier(context, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, context->family, SHARE_EXTERNAL_FAMILY, VK_ACCESS_TRANSFER_READ_BIT, 0U, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	result = share_submit(context);
	if (result != VK_SUCCESS)
		return result;

	/* The oracle cannot substitute a CPU-generated image for the actual imported GPU contents. */
	result = share_verify(context);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: this independent renderer consumed every expected producer pixel. */
	return VK_SUCCESS;
}

/* Allocates a separate host-visible destination for the receiver's GPU copy. */
static VkResult
share_buffer(
	struct share_context *context)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocation;
	VkResult result;
	uint32_t type;
	uint32_t index;

	/* Readback storage is not the exported allocation and is never presented. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = SHARE_BYTES;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	context->operation = "vkCreateBuffer";
	result = vkCreateBuffer(context->device, &create, NULL, &context->readback);
	if (result != VK_SUCCESS)
		return result;

	/* Memory selection honors this actual buffer's supported memory types. */
	vkGetBufferMemoryRequirements(context->device, context->readback, &requirements);
	vkGetPhysicalDeviceMemoryProperties(context->physical, &properties);
	if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Search only the actual bounded Vulkan memory-type inventory. */
	type = UINT32_MAX;
	for (index = 0U; index < properties.memoryTypeCount; index++) {
		/* The standard type mask excludes incompatible heaps even when they are visible. */
		if ((requirements.memoryTypeBits & (1U << index)) == 0U)
			continue;

		/* Explicit invalidation below handles either coherent or noncoherent visible memory. */
		if ((properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
			type = index;
			break;
		}
	}

	/* A device without compatible readable memory cannot supply this oracle. */
	context->operation = "readback memory type";
	if (type == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Allocate and bind the exact storage required by the receiver buffer. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;
	context->operation = "vkAllocateMemory readback";
	result = vkAllocateMemory(context->device, &allocation, NULL, &context->readback_memory);
	if (result != VK_SUCCESS)
		return result;

	/* The copy destination receives an ordinary Vulkan allocation in this context. */
	context->operation = "vkBindBufferMemory";
	result = vkBindBufferMemory(context->device, context->readback, context->readback_memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the receiver owns an independently bound GPU copy destination. */
	return VK_SUCCESS;
}

/* Compares every GPU-copied pixel after explicit mapping and cache invalidation. */
static VkResult
share_verify(
	struct share_context *context)
{
	VkMappedMemoryRange range;
	VkResult result;
	void *mapping;
	const uint8_t *pixels;
	uint32_t offset;
	int mismatch;

	/* Mapping only the oracle buffer cannot make the shared image path CPU-dependent. */
	context->operation = "vkMapMemory readback";
	result = vkMapMemory(context->device, context->readback_memory, 0U, VK_WHOLE_SIZE, 0U, &mapping);
	if (result != VK_SUCCESS)
		return result;

	/* Whole-allocation invalidation satisfies nonCoherentAtomSize alignment without guessing it. */
	memset(&range, 0, sizeof(range));
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = context->readback_memory;
	range.size = VK_WHOLE_SIZE;
	context->operation = "vkInvalidateMappedMemoryRanges";
	result = vkInvalidateMappedMemoryRanges(context->device, 1U, &range);
	if (result != VK_SUCCESS) {
		vkUnmapMemory(context->device, context->readback_memory);
		return result;
	}

	/* Exact endpoint values detect stale, zero-filled and misbound allocation contents. */
	pixels = mapping;
	mismatch = 0;
	for (offset = 0U; offset < SHARE_BYTES; offset += 4U) {
		/* All 1024 independently copied pixels must contain producer-written magenta. */
		if (pixels[offset] != 255U ||
		    pixels[offset + 1U] != 0U ||
		    pixels[offset + 2U] != 255U ||
		    pixels[offset + 3U] != 255U) {
			fprintf(stderr, "GPU SHARE PIXEL FAILED index=%u rgba=%02x%02x%02x%02x\n", offset / 4U, pixels[offset], pixels[offset + 1U], pixels[offset + 2U], pixels[offset + 3U]);
			mismatch = 1;
			break;
		}
	}

	/* CPU mapping ownership ends before either success or mismatch is reported. */
	vkUnmapMemory(context->device, context->readback_memory);
	context->operation = "imported GPU pixel oracle";
	if (mismatch != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: every byte came from the same allocation filled by the exited producer. */
	return VK_SUCCESS;
}

/* Records one explicit ownership and layout transition for the shared image. */
static void
share_barrier(
	struct share_context *context,
	VkImageLayout old_layout,
	VkImageLayout new_layout,
	uint32_t source_family,
	uint32_t destination_family,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;
	VkBufferMemoryBarrier buffer;

	/* A buffer transfers external ownership without inventing image layouts or subresources. */
	if (share_kind == SHARE_BUFFER) {
		memset(&buffer, 0, sizeof(buffer));
		buffer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		buffer.srcAccessMask = source_access;
		buffer.dstAccessMask = destination_access;
		buffer.srcQueueFamilyIndex = source_family;
		buffer.dstQueueFamilyIndex = destination_family;
		buffer.buffer = context->shared_buffer;
		buffer.size = SHARE_BYTES;
		vkCmdPipelineBarrier(context->command, source_stage, destination_stage, 0U, 0U, NULL, 1U, &buffer, 0U, NULL);

		/* Succeeded: the buffer's actual allocation participates in the external ownership boundary. */
		return;
	}

	/* Every transition covers exactly the image's sole color level and layer. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = source_family;
	barrier.dstQueueFamilyIndex = destination_family;
	barrier.image = context->image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(context->command, source_stage, destination_stage, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Succeeded: the barrier is recorded in the owning process's actual command stream. */
	return;
}

/* Sends one typed kernel capability through the ordinary AF_UNIX control-message path. */
static int
share_send_fd(
	int socket,
	int descriptor)
{
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr message;
	struct cmsghdr *header;
	struct iovec vector;
	ssize_t sent;
	char payload;

	/* The byte makes stream delivery and its attached capability one receive operation. */
	payload = 'G';
	vector.iov_base = &payload;
	vector.iov_len = 1U;
	memset(&control, 0, sizeof(control));
	memset(&message, 0, sizeof(message));
	message.msg_iov = &vector;
	message.msg_iovlen = 1U;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	header = (struct cmsghdr *)(void *)control.bytes;
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));

	/* The kernel retains the capability even if the sender closes its local fd immediately. */
	sent = sendmsg(socket, &message, 0);
	if (sent != 1)
		return -1;

	/* Succeeded: the socket message owns an independent allocation reference. */
	return 0;
}

/* Receives exactly one capability and rejects truncation or unrelated ancillary data. */
static int
share_receive_fd(
	int socket,
	int *descriptor)
{
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr message;
	struct cmsghdr *header;
	struct iovec vector;
	ssize_t received;
	char payload;

	/* A zeroed receive record exposes actual returned metadata rather than stale stack bytes. */
	payload = 0;
	vector.iov_base = &payload;
	vector.iov_len = 1U;
	memset(&control, 0, sizeof(control));
	memset(&message, 0, sizeof(message));
	message.msg_iov = &vector;
	message.msg_iovlen = 1U;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	if (received != 1)
		return -1;

	/* The sender's one rights message has a fixed payload, level, type and length. */
	if (message.msg_controllen < CMSG_LEN(sizeof(int)))
		return -1;
	header = (struct cmsghdr *)(void *)control.bytes;
	if (header->cmsg_level != SOL_SOCKET ||
	    header->cmsg_type != SCM_RIGHTS ||
	    header->cmsg_len != CMSG_LEN(sizeof(int)))
		return -1;
	memcpy(descriptor, CMSG_DATA(header), sizeof(*descriptor));

	/* A received descriptor is still returned for cleanup when the accompanying byte is invalid. */
	if (message.msg_flags != 0 || payload != 'G' || *descriptor < 0)
		return -1;

	/* Succeeded: the receiver owns a capability in its independent descriptor namespace. */
	return 0;
}

/* Creates the resource view specified by the test protocol before allocating or importing storage. */
static VkResult
share_allocation_object(
	struct share_context *context,
	VkMemoryRequirements *requirements)
{
	VkExternalMemoryBufferCreateInfo external_buffer;
	VkExternalMemoryImageCreateInfo external_image;
	VkPhysicalDeviceExternalBufferInfo buffer_info;
	VkExternalBufferProperties buffer_properties;
	VkPhysicalDeviceImageFormatInfo2 image_info;
	VkPhysicalDeviceExternalImageFormatInfo image_external;
	VkImageFormatProperties2 image_properties;
	VkExternalImageFormatProperties image_external_properties;
	VkBufferCreateInfo buffer;
	VkImageCreateInfo image;
	VkResult result;

	/* Resource creation uses the renderer's external-memory compatibility rules. */
	if (share_kind == SHARE_BUFFER) {
		memset(&external_buffer, 0, sizeof(external_buffer));
		external_buffer.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
		external_buffer.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

		/* Buffer usage and size are fixed in this finite protocol rather than hidden in a row pitch. */
		memset(&buffer, 0, sizeof(buffer));
		buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer.pNext = &external_buffer;
		buffer.size = SHARE_BYTES;
		buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		/* Standard capability queries must confirm both import and export for this exact usage. */
		memset(&buffer_info, 0, sizeof(buffer_info));
		buffer_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
		buffer_info.usage = buffer.usage;
		buffer_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		memset(&buffer_properties, 0, sizeof(buffer_properties));
		buffer_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
		context->operation = "vkGetPhysicalDeviceExternalBufferPropertiesKHR";
		vkGetPhysicalDeviceExternalBufferPropertiesKHR(context->physical, &buffer_info, &buffer_properties);
		if ((buffer_properties.externalMemoryProperties.externalMemoryFeatures & (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) != (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
			return VK_ERROR_FEATURE_NOT_PRESENT;

		context->operation = "shared vkCreateBuffer";
		result = vkCreateBuffer(context->device, &buffer, NULL, &context->shared_buffer);
		if (result != VK_SUCCESS)
			return result;

		/* Requirements come from this receiver or producer's actual native buffer view. */
		vkGetBufferMemoryRequirements(context->device, context->shared_buffer, requirements);
	} else {
		memset(&external_image, 0, sizeof(external_image));
		external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

		/* Optimal tiling remains opaque to the CPU and is reproduced by the independent receiver. */
		memset(&image, 0, sizeof(image));
		image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image.pNext = &external_image;
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = VK_FORMAT_R8G8B8A8_UNORM;
		image.extent.width = SHARE_EDGE;
		image.extent.height = SHARE_EDGE;
		image.extent.depth = 1U;
		image.mipLevels = 1U;
		image.arrayLayers = 1U;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		/* Optimal-image compatibility comes from the extensible standard format query. */
		memset(&image_external, 0, sizeof(image_external));
		image_external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
		image_external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		memset(&image_info, 0, sizeof(image_info));
		image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		image_info.pNext = &image_external;
		image_info.format = image.format;
		image_info.type = image.imageType;
		image_info.tiling = image.tiling;
		image_info.usage = image.usage;
		memset(&image_external_properties, 0, sizeof(image_external_properties));
		image_external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		memset(&image_properties, 0, sizeof(image_properties));
		image_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		image_properties.pNext = &image_external_properties;
		context->operation = "vkGetPhysicalDeviceImageFormatProperties2KHR";
		result = vkGetPhysicalDeviceImageFormatProperties2KHR(context->physical, &image_info, &image_properties);
		if (result != VK_SUCCESS)
			return result;
		if ((image_external_properties.externalMemoryProperties.externalMemoryFeatures & (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) != (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
			return VK_ERROR_FEATURE_NOT_PRESENT;

		context->operation = "shared optimal vkCreateImage";
		result = vkCreateImage(context->device, &image, NULL, &context->image);
		if (result != VK_SUCCESS)
			return result;

		/* Optimal memory is checked by allocation requirements, never by a fabricated row layout. */
		vkGetImageMemoryRequirements(context->device, context->image, requirements);
	}

	/* Succeeded: the resource view exists without allocating a replacement for imported storage. */
	return VK_SUCCESS;
}

/* Allocates renderer-exportable buffer or optimal storage and publishes its protocol description. */
static VkResult
share_allocation_create(
	struct share_context *context,
	int *fd)
{
	VkExportMemoryAllocateInfo export;
	VkMemoryGetFdInfoKHR get_fd;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocate;
	uint32_t type;
	VkResult result;

	/* Native creation establishes the real resource's memory compatibility mask. */
	memset(&requirements, 0, sizeof(requirements));
	result = share_allocation_object(context, &requirements);
	if (result != VK_SUCCESS)
		return result;

	/* Select a compatible memory type from the actual physical device inventory. */
	vkGetPhysicalDeviceMemoryProperties(context->physical, &properties);
	if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The test needs GPU ownership, not a CPU-visible heap or mapping. */
	for (type = 0U; type < properties.memoryTypeCount; type++) {
		/* Only types supported by this resource may back its shared allocation. */
		if ((requirements.memoryTypeBits & (1U << type)) != 0U)
			break;
	}

	/* An empty compatibility mask cannot be replaced by an arbitrary host memory type. */
	if (type == properties.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Explicit shared allocation creates renderer exportability without a guest CPU view. */
	memset(&allocate, 0, sizeof(allocate));
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &export;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	context->operation = "shared allocation";
	result = vkAllocateMemory(context->device, &allocate, NULL, &context->image_memory);
	if (result != VK_SUCCESS)
		return result;

	/* The resource type determines binding; the exported authority always names the allocation. */
	context->operation = "shared binding";
	if (share_kind == SHARE_BUFFER) {
		result = vkBindBufferMemory(context->device, context->shared_buffer, context->image_memory, 0U);
	} else {
		result = vkBindImageMemory(context->device, context->image, context->image_memory, 0U);
	}

	/* A failed native bind cannot supply a usable cross-process capability. */
	if (result != VK_SUCCESS)
		return result;

	/* The public API supplies a reference-bearing fd without exposing any kernel or Venus ABI. */
	memset(&get_fd, 0, sizeof(get_fd));
	get_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	get_fd.memory = context->image_memory;
	get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	context->operation = "vkGetMemoryFdKHR";
	result = vkGetMemoryFdKHR(context->device, &get_fd, fd);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: only the capability and this allocation retain the shared renderer storage. */
	return VK_SUCCESS;
}

/* Imports standard opaque allocation ownership using the same negotiated resource recipe. */
static VkResult
share_allocation_import(
	struct share_context *context,
	int *fd)
{
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocate;
	VkImportMemoryFdInfoKHR import;
	VkResult result;
	uint32_t type;

	/* Independent creation reproduces the fixed test protocol's buffer or optimal-image view. */
	memset(&requirements, 0, sizeof(requirements));
	result = share_allocation_object(context, &requirements);
	if (result != VK_SUCCESS)
		return result;

	/* The exact same device and recipe select the producer's original allocation size and type. */
	vkGetPhysicalDeviceMemoryProperties(context->physical, &properties);
	for (type = 0U; type < properties.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) != 0U)
			break;
	}
	if (type == properties.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* OPAQUE_FD import validates compatible UUIDs and allocation metadata inside libvulkan. */
	memset(&import, 0, sizeof(import));
	import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	import.fd = *fd;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &import;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	context->operation = "vkAllocateMemory OPAQUE_FD import";
	result = vkAllocateMemory(context->device, &allocate, NULL, &context->image_memory);
	if (result != VK_SUCCESS)
		return result;

	/* Successful standard import consumes the descriptor even if a later bind fails. */
	*fd = -1;
	context->operation = "standard imported binding";
	if (share_kind == SHARE_BUFFER)
		result = vkBindBufferMemory(context->device, context->shared_buffer, context->image_memory, 0U);
	else
		result = vkBindImageMemory(context->device, context->image, context->image_memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: a new process owns the original producer allocation through standard Vulkan APIs. */
	return VK_SUCCESS;
}

/* Finishes producer rendering and destroys the full source context before process exit. */
static int
share_producer(
	int socket)
{
	struct share_context context;
	VkResult result;
	VkResult cleanup;
	int descriptor;
	int sent;

	/* Only this child creates the image and its source renderer namespace. */
	memset(&context, 0, sizeof(context));
	descriptor = -1;
	sent = -1;
	result = share_open(&context);
	if (result == VK_SUCCESS)
		result = share_produce(&context, &descriptor);

	/* A failed GPU operation sends no misleading success capability to the receiver. */
	if (result != VK_SUCCESS) {
		fprintf(stderr, "GPU SHARE FAILED producer api=%s result=%d\n", context.operation, (int)result);
	} else {
		/* Sending after fence completion establishes a real cross-process ownership boundary. */
		sent = share_send_fd(socket, descriptor);
		if (sent != 0)
			perror("gpu-share-client: sendmsg");
	}

	/* The fd and Vulkan memory have separate references; both source references are consumed. */
	if (descriptor >= 0)
		close(descriptor);
	cleanup = share_close(&context);
	if (cleanup != VK_SUCCESS) {
		fprintf(stderr, "GPU SHARE FAILED producer cleanup=%d\n", (int)cleanup);
		return 1;
	}

	/* Process success requires rendering, capability transfer and complete source cleanup. */
	if (result != VK_SUCCESS || sent != 0)
		return 1;

	/* Succeeded: only the socket or receiver retains the actual image allocation. */
	return 0;
}
