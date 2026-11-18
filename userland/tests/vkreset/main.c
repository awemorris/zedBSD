/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkreset: the GPU reset recovery test.  It locks the GPU up on purpose
 * and checks that the device comes back.  It is destructive: the device
 * is reset, so it runs only as "vkreset --isolated", with no other GPU
 * client running (the core resets the device only for a sole fresh open).
 *
 *   1. hang     A compute kernel that never ends (hang.comp with an odd
 *               limit) is submitted on the first device.  The submission
 *               or the fence wait must report VK_ERROR_DEVICE_LOST.
 *   2. close    The lost device and its instance are destroyed; closing
 *               waits for the driver to stop or isolate the context.
 *   3. compute  A fresh instance and device in the same process: the fill
 *               shader of vkcompute (data[x] = 3x + 7, 128 workgroups of 32).
 *   4. clear    A second fresh device: a render pass clears a 64x64 B8G8R8A8
 *               image, copied to a buffer, every pixel compared.
 *
 * It prints one line a step and a total:
 *
 *   vkreset: <step>: PASS (<ms> ms)
 *   vkreset: <step>: FAIL (<call> <VkResult>)
 *   vkreset: PASS | vkreset: FAIL
 *
 * The exit status is 0 only on PASS; 2 when not run with --isolated.  A
 * draw in a new process is the board procedure's (vkmatrix, gpu-selftest).
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "shader.h"
#include "../vkcompute/shader.h"

/* The fill's workgroup size, workgroups and words (vkcompute's fill.comp). */
#define VKRESET_LOCAL_SIZE	32U
#define VKRESET_GROUPS		128U
#define VKRESET_WORDS		(VKRESET_LOCAL_SIZE * VKRESET_GROUPS)

/* The cleared image's side, its pixels and the colour's B8G8R8A8 word. */
#define VKRESET_SIZE		64U
#define VKRESET_PIXELS		(VKRESET_SIZE * VKRESET_SIZE)
#define VKRESET_CLEAR_WORD	0xff336699U

/* The fence waits: the hang's is past the core's execution deadline (60 s) and stop interval (10 s). */
#define VKRESET_HANG_WAIT_NS	120000000000ULL
#define VKRESET_WAIT_NS		10000000000ULL

/* One instance, its device and the objects of one step. */
struct vkreset {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkRenderPass pass;
	VkFramebuffer framebuffer;
	VkShaderModule module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	const char *failed_call;
};

static uint64_t vkreset_ms(void);
static VkResult vkreset_open(struct vkreset *run);
static VkResult vkreset_buffer(struct vkreset *run, VkDeviceSize bytes, VkBufferUsageFlags usage);
static VkResult vkreset_compute(struct vkreset *run, const uint32_t *code, size_t size, uint32_t groups);
static VkResult vkreset_record_begin(struct vkreset *run);
static VkResult vkreset_submit(struct vkreset *run, uint64_t wait_ns);
static VkResult vkreset_clear(struct vkreset *run);
static int vkreset_hang(void);
static int vkreset_fresh(void);
static void vkreset_destroy(struct vkreset *run);

/*
 * Runs the steps in order; a step that fails ends the run.
 */
int
main(
	int argc,
	char **argv)
{
	int isolated;
	int named;
	int failed;

	/* Resets the device: only on explicit request. */
	isolated = 0;
	if (argc == 2) {
		named = strcmp(argv[1], "--isolated");
		if (named == 0)
			isolated = 1;
	}

	if (!isolated) {
		printf("vkreset: usage: vkreset --isolated (no other GPU client may run)\n");
		return 2;
	}

	/* The lockup and its loss, then a fresh device. */
	failed = vkreset_hang();
	if (failed == 0)
		failed = vkreset_fresh();

	/* The total. */
	printf("vkreset: %s\n", failed == 0 ? "PASS" : "FAIL");
	if (failed != 0)
		return 1;

	/* Succeeded: the device was lost and came back. */
	return 0;
}

/* The monotonic clock in milliseconds. */
static uint64_t
vkreset_ms(
	void)
{
	struct timespec now;
	int error;

	/* Reads the clock; a failure reads as zero. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: the milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Steps 1 and 2: submits the kernel that never ends, expects the device
 * to be lost, then destroys everything.  Returns 1 on failure.
 */
static int
vkreset_hang(
	void)
{
	struct vkreset run;
	uint32_t *words;
	uint64_t start;
	VkResult status;
	void *mapped;

	/* The device, a buffer whose limit is odd, and the hang's pipeline. */
	memset(&run, 0, sizeof(run));
	status = vkreset_open(&run);
	if (status == VK_SUCCESS)
		status = vkreset_buffer(&run, 2U * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	if (status == VK_SUCCESS) {
		run.failed_call = "vkMapMemory";
		status = vkMapMemory(run.device, run.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	}

	if (status == VK_SUCCESS) {
		words = mapped;
		words[0] = 1U;
		words[1] = 0U;
		vkUnmapMemory(run.device, run.memory);
		status = vkreset_compute(&run, vkreset_hang_shader, sizeof(vkreset_hang_shader), 1U);
	}

	if (status != VK_SUCCESS) {
		printf("vkreset: hang: FAIL (setup %s %d)\n", run.failed_call, (int)status);
		vkreset_destroy(&run);
		return 1;
	}

	/* The submission: the device must be lost, not finish and not time out. */
	start = vkreset_ms();
	status = vkreset_submit(&run, VKRESET_HANG_WAIT_NS);
	if (status != VK_ERROR_DEVICE_LOST) {
		printf("vkreset: hang: FAIL (%s %d, expected VK_ERROR_DEVICE_LOST %d)\n", run.failed_call, (int)status, (int)VK_ERROR_DEVICE_LOST);
		vkreset_destroy(&run);
		return 1;
	}

	printf("vkreset: hang: PASS (%llu ms, %s VK_ERROR_DEVICE_LOST)\n", (unsigned long long)(vkreset_ms() - start), run.failed_call);

	/* Destroying the lost device and its instance closes the session. */
	start = vkreset_ms();
	vkreset_destroy(&run);
	printf("vkreset: close: PASS (%llu ms)\n", (unsigned long long)(vkreset_ms() - start));

	/* Succeeded: the device was lost and its session closed. */
	return 0;
}

/*
 * Steps 3 and 4: a fresh instance and device run the fill and a clear.
 * Returns 1 on failure.
 */
static int
vkreset_fresh(
	void)
{
	struct vkreset run;
	const uint32_t *words;
	uint64_t start;
	uint32_t index;
	uint32_t good;
	VkResult status;
	void *mapped;

	/* The fresh device (the core resets the hardware for it) and the fill. */
	memset(&run, 0, sizeof(run));
	start = vkreset_ms();
	status = vkreset_open(&run);
	if (status == VK_SUCCESS)
		status = vkreset_buffer(&run, VKRESET_WORDS * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	if (status == VK_SUCCESS)
		status = vkreset_compute(&run, vkcompute_fill_shader, sizeof(vkcompute_fill_shader), VKRESET_GROUPS);
	if (status == VK_SUCCESS)
		status = vkreset_submit(&run, VKRESET_WAIT_NS);
	if (status == VK_SUCCESS) {
		run.failed_call = "vkMapMemory";
		status = vkMapMemory(run.device, run.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	}

	if (status != VK_SUCCESS) {
		printf("vkreset: compute: FAIL (%s %d)\n", run.failed_call, (int)status);
		vkreset_destroy(&run);
		return 1;
	}

	/* Counts the words that hold 3x + 7. */
	words = mapped;
	good = 0;
	for (index = 0; index < VKRESET_WORDS; index++) {
		if (words[index] == index * 3U + 7U)
			good++;
	}

	vkUnmapMemory(run.device, run.memory);

	/* Fails on a word the fill did not write. */
	if (good != VKRESET_WORDS) {
		printf("vkreset: compute: FAIL (%u/%u words)\n", good, VKRESET_WORDS);
		vkreset_destroy(&run);
		return 1;
	}

	printf("vkreset: compute: PASS (%llu ms, %u/%u words)\n", (unsigned long long)(vkreset_ms() - start), good, VKRESET_WORDS);

	/* The clear on a second fresh device, read back through a buffer. */
	vkreset_destroy(&run);
	memset(&run, 0, sizeof(run));
	start = vkreset_ms();
	status = vkreset_open(&run);
	if (status == VK_SUCCESS)
		status = vkreset_buffer(&run, VKRESET_PIXELS * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	if (status == VK_SUCCESS)
		status = vkreset_clear(&run);
	if (status == VK_SUCCESS)
		status = vkreset_submit(&run, VKRESET_WAIT_NS);
	if (status == VK_SUCCESS) {
		run.failed_call = "vkMapMemory";
		status = vkMapMemory(run.device, run.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	}

	if (status != VK_SUCCESS) {
		printf("vkreset: clear: FAIL (%s %d)\n", run.failed_call, (int)status);
		vkreset_destroy(&run);
		return 1;
	}

	/* Counts the pixels of the clear colour. */
	words = mapped;
	good = 0;
	for (index = 0; index < VKRESET_PIXELS; index++) {
		if (words[index] == VKRESET_CLEAR_WORD)
			good++;
	}

	/* Says which pixel was wrong while the memory is still mapped. */
	if (good != VKRESET_PIXELS)
		printf("vkreset: clear: FAIL (%u/%u pixels, first 0x%08x, expected 0x%08x)\n", good, VKRESET_PIXELS, words[0], VKRESET_CLEAR_WORD);

	/* The device goes. */
	vkUnmapMemory(run.device, run.memory);
	vkreset_destroy(&run);

	/* Fails on a pixel the clear did not write. */
	if (good != VKRESET_PIXELS)
		return 1;
	printf("vkreset: clear: PASS (%llu ms, %u/%u pixels)\n", (unsigned long long)(vkreset_ms() - start), good, VKRESET_PIXELS);

	/* Succeeded: compute and a clear ran on the fresh device. */
	return 0;
}

/* Creates an instance and a device with one queue of the first family with graphics and compute. */
static VkResult
vkreset_open(
	struct vkreset *run)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkQueueFamilyProperties families[8];
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	VkQueueFlags wanted;
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult status;

	/* A Vulkan 1.0 instance with no layer and no extension. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkreset";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	run->failed_call = "vkCreateInstance";
	status = vkCreateInstance(&instance_info, NULL, &run->instance);
	if (status != VK_SUCCESS)
		return status;

	/* The first physical device; VK_INCOMPLETE only means there are more. */
	count = 1;
	run->failed_call = "vkEnumeratePhysicalDevices";
	status = vkEnumeratePhysicalDevices(run->instance, &count, &run->physical);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;

	/* Refuses a system without a Vulkan device. */
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Its first family with graphics and compute. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(run->physical, &count, families);
	wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
	run->failed_call = "graphics and compute queue family";
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & wanted) == wanted)
			break;
	}

	if (index == count)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The device with one queue of that family. */
	run->family = index;
	priority = 1.0f;
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = run->family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	run->failed_call = "vkCreateDevice";
	status = vkCreateDevice(run->physical, &device_info, NULL, &run->device);
	if (status != VK_SUCCESS)
		return status;
	vkGetDeviceQueue(run->device, run->family, 0, &run->queue);

	/* Succeeded: the device and its queue are open. */
	return VK_SUCCESS;
}

/* Creates a buffer of a size and usage in host-visible, host-coherent memory. */
static VkResult
vkreset_buffer(
	struct vkreset *run,
	VkDeviceSize bytes,
	VkBufferUsageFlags usage)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryAllocateInfo allocate_info;
	VkMemoryPropertyFlags wanted;
	uint32_t type;
	VkResult status;

	/* The buffer. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = bytes;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	run->failed_call = "vkCreateBuffer";
	status = vkCreateBuffer(run->device, &buffer_info, NULL, &run->buffer);
	if (status != VK_SUCCESS)
		return status;

	/* A type the buffer accepts that the processor sees without flushes. */
	vkGetBufferMemoryRequirements(run->device, run->buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (type = 0; type < memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) != 0 && (memory.memoryTypes[type].propertyFlags & wanted) == wanted)
			break;
	}

	run->failed_call = "host-coherent memory type";
	if (type == memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The memory, bound. */
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &run->memory);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindBufferMemory";
	status = vkBindBufferMemory(run->device, run->buffer, run->memory, 0);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the buffer is bound. */
	return VK_SUCCESS;
}

/* Builds a compute pipeline over the buffer at set 0, binding 0, and records one dispatch. */
static VkResult
vkreset_compute(
	struct vkreset *run,
	const uint32_t *code,
	size_t size,
	uint32_t groups)
{
	VkShaderModuleCreateInfo module_info;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo pipeline_info;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo set_info;
	VkDescriptorBufferInfo buffer_info;
	VkWriteDescriptorSet write;
	VkMemoryBarrier barrier;
	VkResult status;

	/* The module, the set layout of one storage buffer, the layouts and the pipeline. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = size;
	module_info.pCode = code;
	run->failed_call = "vkCreateShaderModule";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &run->module);
	if (status != VK_SUCCESS)
		return status;
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	memset(&set_layout_info, 0, sizeof(set_layout_info));
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings = &binding;
	run->failed_call = "vkCreateDescriptorSetLayout";
	status = vkCreateDescriptorSetLayout(run->device, &set_layout_info, NULL, &run->set_layout);
	if (status != VK_SUCCESS)
		return status;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkCreatePipelineLayout";
	status = vkCreatePipelineLayout(run->device, &layout_info, NULL, &run->pipeline_layout);
	if (status != VK_SUCCESS)
		return status;
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = run->module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = run->pipeline_layout;
	run->failed_call = "vkCreateComputePipelines";
	status = vkCreateComputePipelines(run->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &run->pipeline);
	if (status != VK_SUCCESS)
		return status;

	/* The pool, the set and its write. */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size.descriptorCount = 1;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	run->failed_call = "vkCreateDescriptorPool";
	status = vkCreateDescriptorPool(run->device, &pool_info, NULL, &run->pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool = run->pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkAllocateDescriptorSets";
	status = vkAllocateDescriptorSets(run->device, &set_info, &run->set);
	if (status != VK_SUCCESS)
		return status;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.buffer = run->buffer;
	buffer_info.range = VK_WHOLE_SIZE;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = run->set;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &buffer_info;
	vkUpdateDescriptorSets(run->device, 1, &write, 0, NULL);

	/* The dispatch and the barrier to the host. */
	status = vkreset_record_begin(run);
	if (status != VK_SUCCESS)
		return status;
	vkCmdBindPipeline(run->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline);
	vkCmdBindDescriptorSets(run->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline_layout, 0, 1, &run->set, 0, NULL);
	vkCmdDispatch(run->command, groups, 1, 1);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(run->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	run->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the dispatch is recorded. */
	return VK_SUCCESS;
}

/* Creates the command pool and one command buffer, and begins it. */
static VkResult
vkreset_record_begin(
	struct vkreset *run)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo command_info;
	VkCommandBufferBeginInfo begin;
	VkResult status;

	/* The pool and the buffer. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = run->family;
	run->failed_call = "vkCreateCommandPool";
	status = vkCreateCommandPool(run->device, &pool_info, NULL, &run->command_pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = run->command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	run->failed_call = "vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(run->device, &command_info, &run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Begins it for one submission. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	run->failed_call = "vkBeginCommandBuffer";
	status = vkBeginCommandBuffer(run->command, &begin);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: recording. */
	return VK_SUCCESS;
}

/* Submits the recorded buffer with a fence and waits for it; names the call that failed. */
static VkResult
vkreset_submit(
	struct vkreset *run,
	uint64_t wait_ns)
{
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkResult status;

	/* The fence. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	run->failed_call = "vkCreateFence";
	status = vkCreateFence(run->device, &fence_info, NULL, &run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* The submission. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &run->command;
	run->failed_call = "vkQueueSubmit";
	status = vkQueueSubmit(run->queue, 1, &submit, run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* The wait; VK_TIMEOUT is reported like any failure. */
	run->failed_call = "vkWaitForFences";
	status = vkWaitForFences(run->device, 1, &run->fence, VK_TRUE, wait_ns);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the work finished. */
	return VK_SUCCESS;
}

/* Records a render pass that clears a 64x64 B8G8R8A8 image and a copy of it into the buffer. */
static VkResult
vkreset_clear(
	struct vkreset *run)
{
	VkImageCreateInfo image_info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryAllocateInfo allocate_info;
	VkImageViewCreateInfo view_info;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass_info;
	VkFramebufferCreateInfo framebuffer_info;
	VkRenderPassBeginInfo begin;
	VkClearValue clear;
	VkBufferImageCopy copy;
	VkMemoryBarrier barrier;
	uint32_t type;
	VkResult status;

	/* The image, in device memory, and its view. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	image_info.extent.width = VKRESET_SIZE;
	image_info.extent.height = VKRESET_SIZE;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	run->failed_call = "vkCreateImage";
	status = vkCreateImage(run->device, &image_info, NULL, &run->image);
	if (status != VK_SUCCESS)
		return status;
	vkGetImageMemoryRequirements(run->device, run->image, &requirements);
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	for (type = 0; type < memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) != 0)
			break;
	}

	run->failed_call = "image memory type";
	if (type == memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory (image)";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &run->image_memory);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindImageMemory";
	status = vkBindImageMemory(run->device, run->image, run->image_memory, 0);
	if (status != VK_SUCCESS)
		return status;
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = run->image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	run->failed_call = "vkCreateImageView";
	status = vkCreateImageView(run->device, &view_info, NULL, &run->view);
	if (status != VK_SUCCESS)
		return status;

	/* The pass: cleared, stored, left ready for the copy. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass_info.attachmentCount = 1;
	pass_info.pAttachments = &attachment;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	run->failed_call = "vkCreateRenderPass";
	status = vkCreateRenderPass(run->device, &pass_info, NULL, &run->pass);
	if (status != VK_SUCCESS)
		return status;
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = run->pass;
	framebuffer_info.attachmentCount = 1;
	framebuffer_info.pAttachments = &run->view;
	framebuffer_info.width = VKRESET_SIZE;
	framebuffer_info.height = VKRESET_SIZE;
	framebuffer_info.layers = 1;
	run->failed_call = "vkCreateFramebuffer";
	status = vkCreateFramebuffer(run->device, &framebuffer_info, NULL, &run->framebuffer);
	if (status != VK_SUCCESS)
		return status;

	/* The pass with its clear (B 0x99, G 0x66, R 0x33, A 0xff), the copy and the barrier to the host. */
	status = vkreset_record_begin(run);
	if (status != VK_SUCCESS)
		return status;
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = (float)0x33 / 255.0f;
	clear.color.float32[1] = (float)0x66 / 255.0f;
	clear.color.float32[2] = (float)0x99 / 255.0f;
	clear.color.float32[3] = 1.0f;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = run->pass;
	begin.framebuffer = run->framebuffer;
	begin.renderArea.extent.width = VKRESET_SIZE;
	begin.renderArea.extent.height = VKRESET_SIZE;
	begin.clearValueCount = 1;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass(run->command, &begin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(run->command);
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = VKRESET_SIZE;
	copy.imageExtent.height = VKRESET_SIZE;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(run->command, run->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, run->buffer, 1, &copy);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(run->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	run->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the clear and the copy are recorded. */
	return VK_SUCCESS;
}

/* Destroys every object of a run, newest first, then the instance. */
static void
vkreset_destroy(
	struct vkreset *run)
{
	/* The device's objects; a lost device's wait returns at once. */
	if (run->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(run->device);
		vkDestroyFence(run->device, run->fence, NULL);
		vkDestroyCommandPool(run->device, run->command_pool, NULL);
		vkDestroyFramebuffer(run->device, run->framebuffer, NULL);
		vkDestroyRenderPass(run->device, run->pass, NULL);
		vkDestroyImageView(run->device, run->view, NULL);
		vkDestroyImage(run->device, run->image, NULL);
		vkFreeMemory(run->device, run->image_memory, NULL);
		vkDestroyDescriptorPool(run->device, run->pool, NULL);
		vkDestroyPipeline(run->device, run->pipeline, NULL);
		vkDestroyPipelineLayout(run->device, run->pipeline_layout, NULL);
		vkDestroyDescriptorSetLayout(run->device, run->set_layout, NULL);
		vkDestroyShaderModule(run->device, run->module, NULL);
		vkDestroyBuffer(run->device, run->buffer, NULL);
		vkFreeMemory(run->device, run->memory, NULL);
		vkDestroyDevice(run->device, NULL);
	}

	/* The instance last; it closes the GPU node. */
	if (run->instance != VK_NULL_HANDLE)
		vkDestroyInstance(run->instance, NULL);
	memset(run, 0, sizeof(*run));
}
