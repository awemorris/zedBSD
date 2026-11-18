/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The camvk program: captures YUYV frames from /dev/camN straight into
 * Vulkan memory and turns each into a gray image on the GPU.
 *
 * Every camera buffer is a Vulkan allocation exported as a descriptor and
 * handed to the camera on its first queue (CAM_MEMORY_IMPORT), so the
 * camera writes the frame where the GPU reads it, with no copy.  For each
 * frame a compute dispatch packs the luma bytes into a host-visible
 * buffer; the buffer goes back to the camera only after the dispatch's
 * fence.  When the frame memory is host-visible too, the CPU extracts the
 * same luma from it and every frame is compared byte for byte, which shows
 * the GPU reading this frame and not an older one.  The last frame is
 * written as <output>.pgm (and <output>-cpu.pgm when compared).
 *
 *   camvk [-d /dev/cam0] [-w width] [-h height] [-i interval_100ns]
 *         [-b buffers] [-n frames] [-o path] [-t timeout_ms]
 *
 * The last line is "camvk: PASS ..." or "camvk: FAIL (<reason>)".
 */

#include "userland/tests/camcommon/camcommon.h"
#include "userland/tests/camvk/shader.h"

#include <vulkan/vulkan.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <uapi/camera.h>

/* The defaults of the options. */
#define CAMVK_DEFAULT_DEVICE		"/dev/cam0"
#define CAMVK_DEFAULT_WIDTH		640U
#define CAMVK_DEFAULT_HEIGHT		480U
#define CAMVK_DEFAULT_BUFFERS		4U
#define CAMVK_DEFAULT_FRAMES		30U
#define CAMVK_DEFAULT_TIMEOUT_MS	3000U
#define CAMVK_DEFAULT_OUTPUT		"camvk"

/* The exit statuses: success, a usage error, a mode the host cannot carry, any other failure, no import. */
#define CAMVK_EXIT_OK			0
#define CAMVK_EXIT_USAGE		1
#define CAMVK_EXIT_NO_BANDWIDTH		2
#define CAMVK_EXIT_FAILED		3
#define CAMVK_EXIT_NO_IMPORT		4

/* The shader's workgroup size (local_size_x in luma.comp); one invocation packs four pixels. */
#define CAMVK_LOCAL_SIZE		32U
#define CAMVK_PIXELS_PER_WORD		4U

/* The fence wait's limit in nanoseconds. */
#define CAMVK_WAIT_NS			5000000000ULL

/* The path buffer of an output file. */
#define CAMVK_PATH_MAX			256U

/*
 * What the command line asks for.
 */
struct camvk_options {
	const char *device;
	const char *output;
	uint32_t width;
	uint32_t height;
	uint32_t interval_100ns;
	unsigned buffers;
	unsigned frames;
	unsigned timeout_ms;
};

/*
 * One camera buffer: the Vulkan allocation the camera writes and the
 * descriptor set and command buffer that read it.
 *
 * mapped is the CPU's view of the allocation when its memory type is
 * host-visible, and NULL otherwise.
 */
struct camvk_frame {
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDescriptorSet set;
	VkCommandBuffer command;
	const uint8_t *mapped;
	int queued_once;
};

/*
 * The Vulkan objects of one run.
 *
 * Every handle starts as VK_NULL_HANDLE and camvk_destroy() destroys
 * whatever the run got as far as creating.
 */
struct camvk {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	PFN_vkGetMemoryFdKHR get_memory_fd;
	VkShaderModule module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkCommandPool command_pool;
	VkFence fence;
	VkBuffer gray_buffer;
	VkDeviceMemory gray_memory;
	const uint8_t *gray;
	struct camvk_frame frames[CAM_BUFFERS_MAX];
	unsigned frame_count;
	VkDeviceSize frame_bytes;
	VkDeviceSize gray_bytes;
	uint32_t groups;
	int host_visible;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	const char *failed_call;
};

/*
 * What the capture saw.
 */
struct camvk_tally {
	unsigned frames;
	unsigned compared;
	unsigned mismatched;
	unsigned errors;
	unsigned incomplete;
	unsigned sequence_gaps;
	uint64_t first_ns;
	uint64_t last_ns;
};

static int camvk_parse(int argc, char **argv, struct camvk_options *options);
static void camvk_usage(void);
static void camvk_record(void *record, size_t size);
static uint64_t camvk_now_ns(void);
static int camvk_set_format(int descriptor, const struct camvk_options *options, const struct cam_info *info, struct cam_format *format);
static VkResult camvk_open_device(struct camvk *run);
static VkResult camvk_create_pipeline(struct camvk *run);
static VkResult camvk_create_gray(struct camvk *run);
static VkResult camvk_create_frames(struct camvk *run, unsigned count);
static VkResult camvk_create_frame(struct camvk *run, struct camvk_frame *frame);
static VkResult camvk_record_frame(struct camvk *run, struct camvk_frame *frame);
static VkResult camvk_dispatch(struct camvk *run, struct camvk_frame *frame);
static int camvk_queue(int descriptor, struct camvk *run, unsigned index);
static int camvk_capture(int descriptor, const struct camvk_options *options, const struct cam_format *format, struct camvk *run, struct camvk_tally *tally);
static int camvk_compare(const struct camvk *run, const struct camvk_frame *frame, const struct cam_format *format);
static int camvk_save(const struct camvk_options *options, const struct camvk *run, const struct camvk_frame *frame, const struct cam_format *format);
static int camvk_write_pgm(const char *path, const uint8_t *gray, uint32_t width, uint32_t height);
static void camvk_destroy(struct camvk *run);

/*
 * Captures frames into Vulkan memory as the command line asks.
 */
int
main(
	int argc,
	char **argv)
{
	struct cam_request_buffers request;
	struct camvk_options options;
	struct camvk_tally tally;
	struct cam_format format;
	struct cam_info info;
	struct camvk run;
	double seconds;
	const char *device_name;
	const char *visibility;
	VkResult result;
	int descriptor;
	int status;
	int error;

	/* The options. */
	error = camvk_parse(argc, argv, &options);
	if (error != 0) {
		camvk_usage();
		return CAMVK_EXIT_USAGE;
	}

	/* The node and what it is. */
	descriptor = open(options.device, O_RDWR);
	if (descriptor < 0) {
		printf("camvk: FAIL (%s: %s)\n", options.device, strerror(errno));
		return CAMVK_EXIT_FAILED;
	}

	/* Asks what the camera is. */
	error = camcommon_get_info(descriptor, &info);
	if (error != 0) {
		printf("camvk: FAIL (CAM_GET_INFO: %s)\n", strerror(error));
		close(descriptor);
		return CAMVK_EXIT_FAILED;
	}

	/* Tells what the camera is. */
	camcommon_print_info(&info);

	/* Refuses a camera or a kernel that cannot fill GPU memory. */
	if ((info.capabilities & CAM_CAP_IMPORT_GPU) == 0U) {
		printf("camvk: FAIL (the camera does not import GPU memory)\n");
		close(descriptor);
		return CAMVK_EXIT_NO_IMPORT;
	}

	/* The YUYV format; a mode the host cannot carry has its own status. */
	error = camvk_set_format(descriptor, &options, &info, &format);
	if (error != 0) {
		close(descriptor);
		if (error == ENOSPC)
			return CAMVK_EXIT_NO_BANDWIDTH;
		return CAMVK_EXIT_FAILED;
	}

	/* The Vulkan device, the pipeline and the gray buffer. */
	memset(&run, 0, sizeof(run));
	run.frame_bytes = format.image_bytes;
	run.gray_bytes = (VkDeviceSize)format.width * format.height;
	run.groups = (uint32_t)(run.gray_bytes / CAMVK_PIXELS_PER_WORD / CAMVK_LOCAL_SIZE);
	result = camvk_open_device(&run);
	if (result == VK_SUCCESS)
		result = camvk_create_pipeline(&run);
	if (result == VK_SUCCESS)
		result = camvk_create_gray(&run);

	/* Imported slots, one Vulkan allocation each. */
	camvk_record(&request, sizeof(request));
	request.count = options.buffers;
	request.memory = CAM_MEMORY_IMPORT;
	status = 0;
	if (result == VK_SUCCESS)
		status = ioctl(descriptor, CAM_REQUEST_BUFFERS, &request);
	if (result == VK_SUCCESS &&
	    (status < 0 ||
	     request.count < 2U)) {
		printf("camvk: FAIL (CAM_REQUEST_BUFFERS: %s, %u buffers)\n", strerror(errno), (unsigned)request.count);
		camvk_destroy(&run);
		close(descriptor);
		return CAMVK_EXIT_FAILED;
	}

	/* One Vulkan allocation for each slot the framework gave. */
	if (result == VK_SUCCESS)
		result = camvk_create_frames(&run, request.count);

	/* Reports the Vulkan call that failed before any frame. */
	if (result != VK_SUCCESS) {
		device_name = "no device";
		if (run.name[0] != '\0')
			device_name = run.name;
		printf("camvk: FAIL (%s: %s %d)\n", device_name, run.failed_call, (int)result);
		camvk_destroy(&run);
		close(descriptor);
		return CAMVK_EXIT_FAILED;
	}

	/* Tells the device and whether the frames can be compared by the CPU. */
	visibility = "not host-visible (frames not compared)";
	if (run.host_visible)
		visibility = "host-visible (frames compared)";
	printf("vulkan: %s, %u buffers of %u bytes, %s\n", run.name, run.frame_count, (unsigned)run.frame_bytes, visibility);

	/* The frames. */
	memset(&tally, 0, sizeof(tally));
	error = camvk_capture(descriptor, &options, &format, &run, &tally);

	/* The counters, then everything given back: the camera's slots first, then the memory. */
	(void)camcommon_print_counters(descriptor);
	camvk_record(&request, sizeof(request));
	request.count = 0U;
	request.memory = CAM_MEMORY_IMPORT;
	(void)ioctl(descriptor, CAM_REQUEST_BUFFERS, &request);
	close(descriptor);
	camvk_destroy(&run);

	/* The rate of the frames that arrived. */
	seconds = 0.0;
	if (tally.frames > 1U)
		seconds = (double)(tally.last_ns - tally.first_ns) / 1e9;
	if (seconds > 0.0)
		printf("frames: %u in %.3f s (%.2f fps)\n", tally.frames, seconds, (double)(tally.frames - 1U) / seconds);

	/* Reports a capture that failed or frames that disagreed. */
	if (error != 0) {
		printf("camvk: FAIL (%s)\n", strerror(error));
		return CAMVK_EXIT_FAILED;
	}

	/* Refuses frames the GPU read differently from the CPU, errors and lost frames. */
	if (tally.mismatched != 0U ||
	    tally.errors != 0U ||
	    tally.sequence_gaps != 0U) {
		printf("camvk: FAIL (%u of %u frames differ from the CPU, %u errors, %u frames lost)\n",
		    tally.mismatched, tally.compared, tally.errors, tally.sequence_gaps);
		return CAMVK_EXIT_FAILED;
	}

	/* Succeeded: every frame was read by the GPU as the camera wrote it. */
	printf("camvk: PASS (%u frames, %u compared, %u incomplete)\n", tally.frames, tally.compared, tally.incomplete);
	return CAMVK_EXIT_OK;
}

/* Reads the command line; EINVAL for anything it does not understand. */
static int
camvk_parse(
	int argc,
	char **argv,
	struct camvk_options *options)
{
	const char *option;
	const char *value;
	int index;
	int order;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->device = CAMVK_DEFAULT_DEVICE;
	options->output = CAMVK_DEFAULT_OUTPUT;
	options->width = CAMVK_DEFAULT_WIDTH;
	options->height = CAMVK_DEFAULT_HEIGHT;
	options->buffers = CAMVK_DEFAULT_BUFFERS;
	options->frames = CAMVK_DEFAULT_FRAMES;
	options->timeout_ms = CAMVK_DEFAULT_TIMEOUT_MS;

	/* Every option has a value in the next argument. */
	for (index = 1; index < argc; index++) {
		option = argv[index];
		if (index + 1 >= argc)
			return EINVAL;
		index++;
		value = argv[index];

		/* Takes the device path. */
		order = strcmp(option, "-d");
		if (order == 0) {
			options->device = value;
			continue;
		}

		/* Takes the output path. */
		order = strcmp(option, "-o");
		if (order == 0) {
			options->output = value;
			continue;
		}

		/* Takes the width. */
		order = strcmp(option, "-w");
		if (order == 0) {
			options->width = (uint32_t)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the height. */
		order = strcmp(option, "-h");
		if (order == 0) {
			options->height = (uint32_t)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the frame interval. */
		order = strcmp(option, "-i");
		if (order == 0) {
			options->interval_100ns = (uint32_t)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the buffer count. */
		order = strcmp(option, "-b");
		if (order == 0) {
			options->buffers = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the frame count. */
		order = strcmp(option, "-n");
		if (order == 0) {
			options->frames = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the wait for one frame. */
		order = strcmp(option, "-t");
		if (order == 0) {
			options->timeout_ms = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Refuses an option the tool does not know. */
		return EINVAL;
	}

	/* Two buffers at least, so one is with the camera while the GPU reads the other. */
	if (options->buffers < 2U ||
	    options->buffers > CAM_BUFFERS_MAX ||
	    options->frames == 0U)
		return EINVAL;

	/* Succeeded: the options are read. */
	return 0;
}

/* Tells how to call the program. */
static void
camvk_usage(
	void)
{
	fprintf(stderr,
	    "usage: camvk [-d /dev/cam0] [-w width] [-h height] [-i interval_100ns]\n"
	    "             [-b buffers] [-n frames] [-o path] [-t timeout_ms]\n");
}

/* Zeros a camera record and fills its version and size. */
static void
camvk_record(
	void *record,
	size_t size)
{
	uint32_t *words;

	/* Zeros, then the two leading words. */
	memset(record, 0, size);
	words = record;
	words[0] = CAM_ABI_VERSION;
	words[1] = (uint32_t)size;
}

/* Reads the monotonic clock in nanoseconds. */
static uint64_t
camvk_now_ns(
	void)
{
	struct timespec now;
	int status;

	/* The monotonic clock; 0 when it cannot be read. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Succeeded: the time in nanoseconds. */
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/*
 * Sets the YUYV format and checks that the shader can take it: rows
 * without padding and a whole number of workgroups.
 */
static int
camvk_set_format(
	int descriptor,
	const struct camvk_options *options,
	const struct cam_info *info,
	struct cam_format *format)
{
	const char *format_name;
	uint64_t pixels;
	int status;
	int error;

	/* SET_FORMAT of YUYV. */
	camvk_record(format, sizeof(*format));
	format->format = CAM_FORMAT_YUYV;
	format->width = options->width;
	format->height = options->height;
	format->interval_100ns = options->interval_100ns;
	status = ioctl(descriptor, CAM_SET_FORMAT, format);
	if (status < 0) {
		error = errno;
		if (error == ENOSPC) {
			printf("camvk: FAIL (the host cannot carry this mode: payload %u, host takes up to %u)\n",
			    (unsigned)format->payload_bytes, (unsigned)info->host_payload_max);
		} else {
			printf("camvk: FAIL (CAM_SET_FORMAT: %s)\n", strerror(error));
		}

		/* Reports why the camera refused the format. */
		return error;
	}

	/* Tells the format the camera settled on. */
	format_name = camcommon_format_name(format->format);
	printf("format: %s %ux%u interval %u stride %u image %u payload %u\n", format_name,
	    (unsigned)format->width, (unsigned)format->height, (unsigned)format->interval_100ns,
	    (unsigned)format->stride_bytes, (unsigned)format->image_bytes, (unsigned)format->payload_bytes);

	/* The camera may answer with another format or a padded row. */
	pixels = (uint64_t)format->width * format->height;
	if (format->format != CAM_FORMAT_YUYV ||
	    format->stride_bytes != format->width * 2U ||
	    format->image_bytes < pixels * 2U) {
		printf("camvk: FAIL (the mode is not packed YUYV)\n");
		return EINVAL;
	}

	/* One invocation packs four pixels and the dispatch has no partial workgroup. */
	if (pixels == 0U || pixels % (CAMVK_PIXELS_PER_WORD * CAMVK_LOCAL_SIZE) != 0U) {
		printf("camvk: FAIL (%ux%u is not a multiple of %u pixels)\n", (unsigned)format->width,
		    (unsigned)format->height, CAMVK_PIXELS_PER_WORD * CAMVK_LOCAL_SIZE);
		return EINVAL;
	}

	/* Succeeded: the format is set. */
	return 0;
}

/* Creates the instance and a device with exportable memory on the first compute family. */
static VkResult
camvk_open_device(
	struct camvk *run)
{
	static const char *const instance_extensions[] = {
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
	};
	static const char *const device_extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
	};
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDeviceProperties properties;
	VkQueueFamilyProperties families[8];
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult result;

	/* A Vulkan 1.0 instance with the external memory queries. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "camvk";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	instance_info.enabledExtensionCount = 2U;
	instance_info.ppEnabledExtensionNames = instance_extensions;
	run->failed_call = "vkCreateInstance";
	result = vkCreateInstance(&instance_info, NULL, &run->instance);
	if (result != VK_SUCCESS)
		return result;

	/* The first physical device; VK_INCOMPLETE only means there are more. */
	count = 1U;
	run->failed_call = "vkEnumeratePhysicalDevices";
	result = vkEnumeratePhysicalDevices(run->instance, &count, &run->physical);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Names the device in every later line. */
	vkGetPhysicalDeviceProperties(run->physical, &properties);
	memcpy(run->name, properties.deviceName, sizeof(run->name));
	run->name[sizeof(run->name) - 1U] = '\0';

	/* The dispatch's workgroups must fit the device's limit. */
	run->failed_call = "maxComputeWorkGroupCount";
	if (run->groups > properties.limits.maxComputeWorkGroupCount[0])
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The first family with compute. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(run->physical, &count, families);
	run->failed_call = "compute queue family";
	for (index = 0U; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U)
			break;
	}

	/* Refuses a device without a compute queue. */
	if (index == count)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The device with external memory fds and one queue of that family. */
	run->family = index;
	priority = 1.0f;
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = run->family;
	queue_info.queueCount = 1U;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1U;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = 2U;
	device_info.ppEnabledExtensionNames = device_extensions;
	run->failed_call = "vkCreateDevice";
	result = vkCreateDevice(run->physical, &device_info, NULL, &run->device);
	if (result != VK_SUCCESS)
		return result;

	/* The queue and the export entry point. */
	vkGetDeviceQueue(run->device, run->family, 0U, &run->queue);
	run->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(run->device, "vkGetMemoryFdKHR");
	run->failed_call = "vkGetMemoryFdKHR";
	if (run->get_memory_fd == NULL)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	/* Succeeded: the device and its queue are open. */
	return VK_SUCCESS;
}

/* Creates the shader module, the layouts, the pipeline, the pools and the fence. */
static VkResult
camvk_create_pipeline(
	struct camvk *run)
{
	VkShaderModuleCreateInfo module_info;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo pipeline_info;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	VkCommandPoolCreateInfo command_pool_info;
	VkFenceCreateInfo fence_info;
	VkResult result;

	/* The module from the embedded SPIR-V. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = sizeof(camvk_luma_shader);
	module_info.pCode = camvk_luma_shader;
	run->failed_call = "vkCreateShaderModule";
	result = vkCreateShaderModule(run->device, &module_info, NULL, &run->module);
	if (result != VK_SUCCESS)
		return result;

	/* Set 0: the frame at binding 0 and the gray image at binding 1. */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0U;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1U;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1] = bindings[0];
	bindings[1].binding = 1U;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 2U;
	set_info.pBindings = bindings;
	run->failed_call = "vkCreateDescriptorSetLayout";
	result = vkCreateDescriptorSetLayout(run->device, &set_info, NULL, &run->set_layout);
	if (result != VK_SUCCESS)
		return result;

	/* The pipeline layout with that set and no push constant. */
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1U;
	layout_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkCreatePipelineLayout";
	result = vkCreatePipelineLayout(run->device, &layout_info, NULL, &run->pipeline_layout);
	if (result != VK_SUCCESS)
		return result;

	/* The compute pipeline from the module's main. */
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = run->module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = run->pipeline_layout;
	run->failed_call = "vkCreateComputePipelines";
	result = vkCreateComputePipelines(run->device, VK_NULL_HANDLE, 1U, &pipeline_info, NULL, &run->pipeline);
	if (result != VK_SUCCESS)
		return result;

	/* A descriptor pool with one set per camera buffer. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = 2U * CAM_BUFFERS_MAX;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = CAM_BUFFERS_MAX;
	pool_info.poolSizeCount = 1U;
	pool_info.pPoolSizes = &size;
	run->failed_call = "vkCreateDescriptorPool";
	result = vkCreateDescriptorPool(run->device, &pool_info, NULL, &run->pool);
	if (result != VK_SUCCESS)
		return result;

	/* The command pool and the fence every dispatch signals. */
	memset(&command_pool_info, 0, sizeof(command_pool_info));
	command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_info.queueFamilyIndex = run->family;
	run->failed_call = "vkCreateCommandPool";
	result = vkCreateCommandPool(run->device, &command_pool_info, NULL, &run->command_pool);
	if (result != VK_SUCCESS)
		return result;
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	run->failed_call = "vkCreateFence";
	result = vkCreateFence(run->device, &fence_info, NULL, &run->fence);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the pipeline is ready to bind. */
	return VK_SUCCESS;
}

/* Creates the gray buffer in host-visible, host-coherent memory and maps it. */
static VkResult
camvk_create_gray(
	struct camvk *run)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryAllocateInfo allocate_info;
	VkMemoryPropertyFlags wanted;
	uint32_t type;
	void *mapped;
	VkResult result;

	/* One byte per pixel. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = run->gray_bytes;
	buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	run->failed_call = "vkCreateBuffer (gray)";
	result = vkCreateBuffer(run->device, &buffer_info, NULL, &run->gray_buffer);
	if (result != VK_SUCCESS)
		return result;

	/* A type the CPU reads without flushes. */
	vkGetBufferMemoryRequirements(run->device, run->gray_buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (type = 0U; type < memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) == 0U)
			continue;
		if ((memory.memoryTypes[type].propertyFlags & wanted) == wanted)
			break;
	}

	/* Refuses a device without host-coherent memory for the gray buffer. */
	run->failed_call = "host-coherent memory type";
	if (type == memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The memory, bound and mapped for the whole run. */
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory (gray)";
	result = vkAllocateMemory(run->device, &allocate_info, NULL, &run->gray_memory);
	if (result != VK_SUCCESS)
		return result;
	run->failed_call = "vkBindBufferMemory (gray)";
	result = vkBindBufferMemory(run->device, run->gray_buffer, run->gray_memory, 0U);
	if (result != VK_SUCCESS)
		return result;
	run->failed_call = "vkMapMemory (gray)";
	result = vkMapMemory(run->device, run->gray_memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the CPU reads what the shader writes. */
	run->gray = mapped;
	return VK_SUCCESS;
}

/* Creates one exportable frame allocation per camera slot. */
static VkResult
camvk_create_frames(
	struct camvk *run,
	unsigned count)
{
	VkResult result;
	unsigned index;

	/* Each slot's buffer, memory, set and command buffer. */
	for (index = 0U; index < count; index++) {
		result = camvk_create_frame(run, &run->frames[index]);
		if (result != VK_SUCCESS)
			return result;
		run->frame_count = index + 1U;
	}

	/* Succeeded: every slot has its memory. */
	return VK_SUCCESS;
}

/*
 * Creates a storage buffer of one frame in exportable memory, the set
 * that names it with the gray buffer, and the command buffer that reads it.
 *
 * A host-visible memory type is preferred, so the CPU can check the GPU.
 */
static VkResult
camvk_create_frame(
	struct camvk *run,
	struct camvk_frame *frame)
{
	VkExternalMemoryBufferCreateInfo external;
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties memory;
	VkExportMemoryAllocateInfo export;
	VkMemoryAllocateInfo allocate_info;
	VkDescriptorSetAllocateInfo set_info;
	VkCommandBufferAllocateInfo command_info;
	uint32_t visible;
	uint32_t type;
	void *mapped;
	VkResult result;

	/* A buffer of one frame that may be shared through an fd. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.pNext = &external;
	buffer_info.size = run->frame_bytes;
	buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	run->failed_call = "vkCreateBuffer (frame)";
	result = vkCreateBuffer(run->device, &buffer_info, NULL, &frame->buffer);
	if (result != VK_SUCCESS)
		return result;

	/* The first host-visible type the buffer takes, else the first type it takes. */
	vkGetBufferMemoryRequirements(run->device, frame->buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	visible = memory.memoryTypeCount;
	for (type = 0U; type < memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) == 0U)
			continue;
		if ((memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
			visible = type;
			break;
		}
	}

	/* Uses the host-visible type when there is one, else any type the buffer fits. */
	if (visible == memory.memoryTypeCount) {
		for (type = 0U; type < memory.memoryTypeCount; type++) {
			if ((requirements.memoryTypeBits & (1U << type)) != 0U)
				break;
		}
	} else {
		type = visible;
	}

	/* Refuses a buffer that fits no memory type. */
	run->failed_call = "frame memory type";
	if (type == memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The allocation, exportable as an fd. */
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.pNext = &export;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory (frame)";
	result = vkAllocateMemory(run->device, &allocate_info, NULL, &frame->memory);
	if (result != VK_SUCCESS)
		return result;
	run->failed_call = "vkBindBufferMemory (frame)";
	result = vkBindBufferMemory(run->device, frame->buffer, frame->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* The CPU's view, when the type has one; every frame then needs it. */
	frame->mapped = NULL;
	if (type == visible) {
		run->failed_call = "vkMapMemory (frame)";
		result = vkMapMemory(run->device, frame->memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
		if (result != VK_SUCCESS)
			return result;
		frame->mapped = mapped;
	}

	/* Remembers whether the CPU can read the frames. */
	run->host_visible = frame->mapped != NULL;

	/* The set that names this frame and the gray buffer. */
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool = run->pool;
	set_info.descriptorSetCount = 1U;
	set_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkAllocateDescriptorSets";
	result = vkAllocateDescriptorSets(run->device, &set_info, &frame->set);
	if (result != VK_SUCCESS)
		return result;

	/* The command buffer, recorded once and submitted for every frame in the slot. */
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = run->command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1U;
	run->failed_call = "vkAllocateCommandBuffers";
	result = vkAllocateCommandBuffers(run->device, &command_info, &frame->command);
	if (result != VK_SUCCESS)
		return result;

	/* Records the dispatch the frame is read with. */
	result = camvk_record_frame(run, frame);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the frame's memory waits for its first queue. */
	return VK_SUCCESS;
}

/* Points a frame's set at its buffers and records its dispatch. */
static VkResult
camvk_record_frame(
	struct camvk *run,
	struct camvk_frame *frame)
{
	VkDescriptorBufferInfo buffers[2];
	VkWriteDescriptorSet write;
	VkCommandBufferBeginInfo begin;
	VkMemoryBarrier barrier;
	VkResult result;

	/* Binding 0 is the frame, binding 1 the gray buffer. */
	memset(buffers, 0, sizeof(buffers));
	buffers[0].buffer = frame->buffer;
	buffers[0].range = VK_WHOLE_SIZE;
	buffers[1].buffer = run->gray_buffer;
	buffers[1].range = VK_WHOLE_SIZE;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = frame->set;
	write.dstBinding = 0U;
	write.descriptorCount = 2U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = buffers;
	vkUpdateDescriptorSets(run->device, 1U, &write, 0U, NULL);

	/*
	 * The camera's CPU writes are made visible to the shader by the
	 * submission itself; the barrier after the dispatch makes the shader's
	 * writes visible to the host read.
	 */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	run->failed_call = "vkBeginCommandBuffer";
	result = vkBeginCommandBuffer(frame->command, &begin);
	if (result != VK_SUCCESS)
		return result;
	vkCmdBindPipeline(frame->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline);
	vkCmdBindDescriptorSets(frame->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline_layout, 0U, 1U, &frame->set, 0U, NULL);
	vkCmdDispatch(frame->command, run->groups, 1U, 1U);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(
		frame->command,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT,
		0U,
		1U,
		&barrier,
		0U,
		NULL,
		0U,
		NULL);
	run->failed_call = "vkEndCommandBuffer";
	result = vkEndCommandBuffer(frame->command);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the command buffer can be submitted again and again. */
	return VK_SUCCESS;
}

/* Runs one frame's dispatch and waits for it. */
static VkResult
camvk_dispatch(
	struct camvk *run,
	struct camvk_frame *frame)
{
	VkSubmitInfo submit;
	VkResult result;

	/* Submits the frame's command buffer with the fence. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &frame->command;
	run->failed_call = "vkQueueSubmit";
	result = vkQueueSubmit(run->queue, 1U, &submit, run->fence);
	if (result != VK_SUCCESS)
		return result;

	/* Waits for the dispatch; only then may the camera write the frame again. */
	run->failed_call = "vkWaitForFences";
	result = vkWaitForFences(run->device, 1U, &run->fence, VK_TRUE, CAMVK_WAIT_NS);
	if (result != VK_SUCCESS)
		return result;
	run->failed_call = "vkResetFences";
	result = vkResetFences(run->device, 1U, &run->fence);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the gray buffer holds this frame's luma. */
	return VK_SUCCESS;
}

/*
 * Queues a slot.  The first queue hands the camera the frame's memory as
 * a descriptor; the camera keeps its own reference, so the descriptor is
 * closed right after.
 */
static int
camvk_queue(
	int descriptor,
	struct camvk *run,
	unsigned index)
{
	VkMemoryGetFdInfoKHR get_fd;
	struct camvk_frame *frame;
	struct cam_buffer buffer;
	VkResult result;
	int memory_fd;
	int status;
	int error;

	/* The memory's descriptor, for the first queue only. */
	frame = &run->frames[index];
	memory_fd = -1;
	if (!frame->queued_once) {
		memset(&get_fd, 0, sizeof(get_fd));
		get_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
		get_fd.memory = frame->memory;
		get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		result = run->get_memory_fd(run->device, &get_fd, &memory_fd);
		if (result != VK_SUCCESS) {
			printf("camvk: vkGetMemoryFdKHR of buffer %u: %d\n", index, (int)result);
			return EIO;
		}
	}

	/* QUEUE_BUFFER, then the descriptor is no longer needed here. */
	camvk_record(&buffer, sizeof(buffer));
	buffer.index = index;
	buffer.memory = CAM_MEMORY_IMPORT;
	buffer.fd = memory_fd;
	status = ioctl(descriptor, CAM_QUEUE_BUFFER, &buffer);
	error = errno;
	if (memory_fd >= 0)
		close(memory_fd);

	/* EXDEV is memory the camera cannot write: not one contiguous uncached run. */
	if (status < 0) {
		printf("camvk: CAM_QUEUE_BUFFER %u: %s\n", index, strerror(error));
		return error;
	}

	/* Succeeded: the slot holds the memory and waits for a frame. */
	frame->queued_once = 1;
	return 0;
}

/*
 * Streams the frames: each one is dispatched, compared when it can be,
 * and queued again after its fence.
 */
static int
camvk_capture(
	int descriptor,
	const struct camvk_options *options,
	const struct cam_format *format,
	struct camvk *run,
	struct camvk_tally *tally)
{
	struct cam_buffer buffer;
	struct camvk_frame *frame;
	uint32_t last_sequence;
	unsigned index;
	VkResult result;
	int differs;
	int status;
	int error;

	/* Every slot queued with its memory, then the stream. */
	for (index = 0U; index < run->frame_count; index++) {
		error = camvk_queue(descriptor, run, index);
		if (error != 0)
			return error;
	}

	/* Starts the stream. */
	status = ioctl(descriptor, CAM_STREAM_ON, NULL);
	if (status < 0) {
		error = errno;
		printf("camvk: CAM_STREAM_ON: %s\n", strerror(error));
		return error;
	}

	/* Each frame. */
	error = 0;
	last_sequence = 0U;
	frame = NULL;
	for (index = 0U; index < options->frames; index++) {
		/* The next finished frame. */
		camvk_record(&buffer, sizeof(buffer));
		buffer.memory = CAM_MEMORY_IMPORT;
		buffer.timeout_ms = options->timeout_ms;
		status = ioctl(descriptor, CAM_DEQUEUE_BUFFER, &buffer);
		if (status < 0) {
			error = errno;
			printf("camvk: CAM_DEQUEUE_BUFFER: %s\n", strerror(error));
			break;
		}

		/* The frame's metadata: time, errors and lost frames. */
		if (tally->frames == 0U)
			tally->first_ns = camvk_now_ns();
		tally->last_ns = camvk_now_ns();
		if (tally->frames != 0U && buffer.metadata.sequence != last_sequence + 1U)
			tally->sequence_gaps += buffer.metadata.sequence - last_sequence - 1U;
		last_sequence = buffer.metadata.sequence;
		tally->frames++;
		if ((buffer.metadata.flags & CAM_FRAME_ERROR) != 0U)
			tally->errors++;
		if ((buffer.metadata.flags & CAM_FRAME_INCOMPLETE) != 0U)
			tally->incomplete++;

		/* The GPU reads the frame. */
		frame = &run->frames[buffer.index];
		result = camvk_dispatch(run, frame);
		if (result != VK_SUCCESS) {
			printf("camvk: %s %d\n", run->failed_call, (int)result);
			error = EIO;
			break;
		}

		/* The CPU reads the same memory and checks the GPU's bytes. */
		if (frame->mapped != NULL) {
			tally->compared++;
			differs = camvk_compare(run, frame, format);
			if (differs != 0)
				tally->mismatched++;
		}

		/* Tells the frame. */
		printf("frame %u: buffer %u sequence %u bytes %u flags 0x%x\n", index, (unsigned)buffer.index,
		    (unsigned)buffer.metadata.sequence, (unsigned)buffer.metadata.used_bytes, (unsigned)buffer.metadata.flags);

		/* The last frame stays out of the queue so it can be saved. */
		if (index + 1U == options->frames)
			break;

		/* Gives the slot back to the camera. */
		error = camvk_queue(descriptor, run, buffer.index);
		if (error != 0)
			break;
	}

	/* The stream stops before the last frame is written out. */
	(void)ioctl(descriptor, CAM_STREAM_OFF, NULL);
	if (error == 0 && frame != NULL)
		error = camvk_save(options, run, frame, format);

	/* Reports a stream that broke off. */
	if (error != 0)
		return error;

	/* Succeeded: every frame went through the GPU. */
	return 0;
}

/* Compares the GPU's gray bytes with the luma the CPU reads; 1 on a difference. */
static int
camvk_compare(
	const struct camvk *run,
	const struct camvk_frame *frame,
	const struct cam_format *format)
{
	uint64_t pixels;
	uint64_t pixel;

	/* Every pixel: Y is every other byte of YUYV. */
	pixels = (uint64_t)format->width * format->height;
	for (pixel = 0U; pixel < pixels; pixel++) {
		if (run->gray[pixel] != frame->mapped[pixel * 2U]) {
			printf("camvk: pixel %llu: GPU 0x%02x, CPU 0x%02x\n", (unsigned long long)pixel, run->gray[pixel],
			    frame->mapped[pixel * 2U]);
			return 1;
		}
	}

	/* Succeeded: the GPU read this frame. */
	return 0;
}

/* Writes the last frame's gray image, and the CPU's when it was compared. */
static int
camvk_save(
	const struct camvk_options *options,
	const struct camvk *run,
	const struct camvk_frame *frame,
	const struct cam_format *format)
{
	char path[CAMVK_PATH_MAX];
	uint8_t *gray;
	uint64_t pixels;
	uint64_t pixel;
	int error;

	/* The GPU's image. */
	snprintf(path, sizeof(path), "%s.pgm", options->output);
	error = camvk_write_pgm(path, run->gray, format->width, format->height);
	if (error != 0)
		return error;
	printf("wrote %s\n", path);

	/* Nothing more without the CPU's view. */
	if (frame->mapped == NULL)
		return 0;

	/* The CPU's image of the same frame. */
	pixels = (uint64_t)format->width * format->height;
	gray = malloc((size_t)pixels);
	if (gray == NULL)
		return ENOMEM;
	for (pixel = 0U; pixel < pixels; pixel++)
		gray[pixel] = frame->mapped[pixel * 2U];
	snprintf(path, sizeof(path), "%s-cpu.pgm", options->output);
	error = camvk_write_pgm(path, gray, format->width, format->height);
	free(gray);
	if (error != 0)
		return error;
	printf("wrote %s\n", path);

	/* Succeeded: both images are written. */
	return 0;
}

/* Writes 8-bit gray as a binary PGM (P5) file. */
static int
camvk_write_pgm(
	const char *path,
	const uint8_t *gray,
	uint32_t width,
	uint32_t height)
{
	char header[64];
	FILE *file;
	size_t bytes;
	size_t written;
	int length;
	int closed;
	int error;

	/* The header and the pixels. */
	length = snprintf(header, sizeof(header), "P5\n%u %u\n255\n", (unsigned)width, (unsigned)height);
	file = fopen(path, "wb");
	if (file == NULL) {
		error = errno;
		printf("camvk: %s: %s\n", path, strerror(error));
		return error;
	}

	/* Writes the header, and the pixels only after it was written whole. */
	bytes = (size_t)width * height;
	error = 0;
	written = fwrite(header, 1U, (size_t)length, file);
	if (written != (size_t)length) {
		error = EIO;
	} else {
		written = fwrite(gray, 1U, bytes, file);
		if (written != bytes)
			error = EIO;
	}

	/* Closes the file, which can also report a failed write. */
	closed = fclose(file);
	if (closed != 0 && error == 0)
		error = EIO;

	/* Reports a short write. */
	if (error != 0) {
		printf("camvk: %s: %s\n", path, strerror(error));
		return error;
	}

	/* Succeeded: the image is on disk. */
	return 0;
}

/* Destroys every object the run created, newest first. */
static void
camvk_destroy(
	struct camvk *run)
{
	unsigned index;

	/* Leaves the instance alone when no device was created. */
	if (run->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(run->device);
		for (index = 0U; index < CAM_BUFFERS_MAX; index++) {
			vkDestroyBuffer(run->device, run->frames[index].buffer, NULL);
			vkFreeMemory(run->device, run->frames[index].memory, NULL);
		}

		/* The gray buffer, the fence and the pools, then the pipeline. */
		vkDestroyBuffer(run->device, run->gray_buffer, NULL);
		vkFreeMemory(run->device, run->gray_memory, NULL);
		vkDestroyFence(run->device, run->fence, NULL);
		vkDestroyCommandPool(run->device, run->command_pool, NULL);
		vkDestroyDescriptorPool(run->device, run->pool, NULL);
		vkDestroyPipeline(run->device, run->pipeline, NULL);
		vkDestroyPipelineLayout(run->device, run->pipeline_layout, NULL);
		vkDestroyDescriptorSetLayout(run->device, run->set_layout, NULL);
		vkDestroyShaderModule(run->device, run->module, NULL);
		vkDestroyDevice(run->device, NULL);
	}

	/* Destroys the instance last. */
	if (run->instance != VK_NULL_HANDLE)
		vkDestroyInstance(run->instance, NULL);
}
