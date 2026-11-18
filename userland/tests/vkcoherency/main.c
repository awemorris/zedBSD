/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkcoherency: the CPU and GPU cache coherency stress.  On
 * the first physical device it runs, for every host-visible memory type
 * the buffers accept:
 *
 *   buffer  VKCOHERENCY_BUFFER_ROUNDS times: the CPU writes a new pattern
 *           into the mapped input (flushed when the type is not coherent),
 *           a compute dispatch writes out[x] = in[x] + 3x into the mapped
 *           output (which the CPU filled with another value first), and
 *           the CPU reads the output back (invalidated when the type is
 *           not coherent) and compares every word;
 *
 * and, once, VKCOHERENCY_IMAGE_ROUNDS times: a clear of an OPTIMAL image
 * to a new colour, copied into a mapped buffer the CPU filled first, read
 * back and compared.  A word that holds the CPU's fill or the previous
 * round's value is stale; any other wrong word is wrong.  It prints a line
 * per part and a verdict:
 *
 *   vkcoherency: <device>: buffer type <n> (<coherent|non-coherent>): <rounds> rounds, <stale> stale, <wrong> wrong[, first at round <r> word <w>: 0x<seen>, expected 0x<wanted>]
 *   vkcoherency: <device>: image: <rounds> rounds, <stale> stale, <wrong> wrong[, ...]
 *   vkcoherency: <device>: PASS | FAIL[, <call> <VkResult>]
 *   vkcoherency: no Vulkan device (<call> <VkResult>)
 *
 * The exit status is 0 only on PASS.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shader.h"

/* The shader's workgroup size, the workgroups, and the words of a buffer. */
#define VKCOHERENCY_LOCAL_SIZE	32U
#define VKCOHERENCY_GROUPS	128U
#define VKCOHERENCY_WORDS	(VKCOHERENCY_LOCAL_SIZE * VKCOHERENCY_GROUPS)

/* The rounds of each part. */
#define VKCOHERENCY_BUFFER_ROUNDS	1000U
#define VKCOHERENCY_IMAGE_ROUNDS	200U

/* The image's side (pixels) and its pixels. */
#define VKCOHERENCY_SIDE	64U
#define VKCOHERENCY_PIXELS	(VKCOHERENCY_SIDE * VKCOHERENCY_SIDE)

/* The fence wait's limit in nanoseconds (5 s, above the driver's 2 s). */
#define VKCOHERENCY_WAIT_NS	5000000000ULL

/* The memory types the buffer part tries at most. */
#define VKCOHERENCY_TYPES_MAX	VK_MAX_MEMORY_TYPES

/* One mapped buffer and its memory. */
struct mapped_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t *words;
	VkDeviceSize size;
	int coherent;
};

/* What a part saw. */
struct tally {
	uint32_t rounds;
	uint32_t stale;
	uint32_t wrong;
	int failed;
	uint32_t first_round;
	uint32_t first_word;
	uint32_t first_seen;
	uint32_t first_wanted;
};

/* The Vulkan objects of one run; every handle starts null and is destroyed at the end. */
struct vkcoherency {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkPhysicalDeviceMemoryProperties memory;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkShaderModule module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	VkImage image;
	VkDeviceMemory image_memory;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	const char *failed_call;
};

static VkResult open_device(struct vkcoherency *run);
static VkResult create_pipeline(struct vkcoherency *run);
static VkResult create_buffer(struct vkcoherency *run, VkBufferUsageFlags usage, uint32_t type, VkDeviceSize size, struct mapped_buffer *mapped);
static void destroy_buffer(struct vkcoherency *run, struct mapped_buffer *mapped);
static VkResult submit_and_wait(struct vkcoherency *run);
static VkResult flush_or_invalidate(struct vkcoherency *run, const struct mapped_buffer *mapped, int flush);
static VkResult run_buffer_part(struct vkcoherency *run, uint32_t type, struct tally *tally);
static VkResult run_image_part(struct vkcoherency *run, struct tally *tally);
static void note(struct tally *tally, uint32_t round, uint32_t word, uint32_t seen, uint32_t wanted, int stale);
static void print_tally(const struct vkcoherency *run, const char *part, const struct tally *tally);
static uint32_t pattern(uint32_t round, uint32_t word);
static void destroy(struct vkcoherency *run);

/* Runs both parts and prints the lines and the verdict. */
int
main(
	void)
{
	struct vkcoherency run;
	struct tally tally;
	VkBufferCreateInfo probe_info;
	VkBuffer probe;
	VkMemoryRequirements requirements;
	char part[64];
	const char *kind;
	uint32_t type;
	int failed;
	VkResult status;

	/* Opens the instance and the device; with none, nothing else can run. */
	memset(&run, 0, sizeof(run));
	status = open_device(&run);
	if (status != VK_SUCCESS && run.name[0] == '\0') {
		printf("vkcoherency: no Vulkan device (%s %d)\n", run.failed_call, (int)status);
		destroy(&run);
		return 1;
	}

	if (status == VK_SUCCESS)
		status = create_pipeline(&run);

	/* The memory types a storage buffer accepts, learnt from a probe buffer. */
	memset(&requirements, 0, sizeof(requirements));
	if (status == VK_SUCCESS) {
		memset(&probe_info, 0, sizeof(probe_info));
		probe_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		probe_info.size = VKCOHERENCY_WORDS * sizeof(uint32_t);
		probe_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		probe_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		run.failed_call = "vkCreateBuffer";
		status = vkCreateBuffer(run.device, &probe_info, NULL, &probe);
		if (status == VK_SUCCESS) {
			vkGetBufferMemoryRequirements(run.device, probe, &requirements);
			vkDestroyBuffer(run.device, probe, NULL);
		}
	}

	/* The buffer part on every host-visible type the buffer accepts. */
	failed = 0;
	for (type = 0; status == VK_SUCCESS && type < run.memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) == 0)
			continue;
		if ((run.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
			continue;
		memset(&tally, 0, sizeof(tally));
		status = run_buffer_part(&run, type, &tally);
		kind = "non-coherent";
		if ((run.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
			kind = "coherent";
		snprintf(part, sizeof(part), "buffer type %u (%s)", type, kind);
		print_tally(&run, part, &tally);
		failed |= tally.failed;
	}

	/* The image part. */
	if (status == VK_SUCCESS) {
		memset(&tally, 0, sizeof(tally));
		status = run_image_part(&run, &tally);
		print_tally(&run, "image", &tally);
		failed |= tally.failed;
	}

	/* The verdict. */
	if (status != VK_SUCCESS)
		printf("vkcoherency: %s: FAIL, %s %d\n", run.name, run.failed_call, (int)status);
	else if (failed)
		printf("vkcoherency: %s: FAIL\n", run.name);
	else
		printf("vkcoherency: %s: PASS\n", run.name);
	destroy(&run);

	/* A failed call or a stale or wrong word fails the run. */
	if (status != VK_SUCCESS || failed)
		return 1;

	/* Succeeded: no stale or wrong word. */
	return 0;
}

/* Creates the instance and the device on the first physical device's compute family. */
static VkResult
open_device(
	struct vkcoherency *run)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDeviceProperties properties;
	VkQueueFamilyProperties families[8];
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult status;

	/* A Vulkan 1.0 instance with no layer and no extension. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkcoherency";
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
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	vkGetPhysicalDeviceProperties(run->physical, &properties);
	memcpy(run->name, properties.deviceName, sizeof(run->name));
	run->name[sizeof(run->name) - 1] = '\0';
	vkGetPhysicalDeviceMemoryProperties(run->physical, &run->memory);

	/* The first family with compute (it also does transfers). */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(run->physical, &count, families);
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
			break;
	}

	run->failed_call = "compute queue family";
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

/* Creates the shader module, the layouts, the pipeline, the descriptor set, the command buffer and the fence. */
static VkResult
create_pipeline(
	struct vkcoherency *run)
{
	VkShaderModuleCreateInfo module_info;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo pipeline_info;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo allocate_info;
	VkCommandPoolCreateInfo command_pool_info;
	VkCommandBufferAllocateInfo command_info;
	VkFenceCreateInfo fence_info;
	uint32_t i;
	VkResult status;

	/* The module from the embedded SPIR-V. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = sizeof(vkcoherency_xform_shader);
	module_info.pCode = vkcoherency_xform_shader;
	run->failed_call = "vkCreateShaderModule";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &run->module);
	if (status != VK_SUCCESS)
		return status;

	/* Set 0: the input at binding 0, the output at binding 1. */
	memset(bindings, 0, sizeof(bindings));
	for (i = 0; i < 2; i++) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 2;
	set_info.pBindings = bindings;
	run->failed_call = "vkCreateDescriptorSetLayout";
	status = vkCreateDescriptorSetLayout(run->device, &set_info, NULL, &run->set_layout);
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

	/* The compute pipeline from the module's main. */
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

	/* The pool and the one set. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	run->failed_call = "vkCreateDescriptorPool";
	status = vkCreateDescriptorPool(run->device, &pool_info, NULL, &run->pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate_info.descriptorPool = run->pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkAllocateDescriptorSets";
	status = vkAllocateDescriptorSets(run->device, &allocate_info, &run->set);
	if (status != VK_SUCCESS)
		return status;

	/* The command pool (each round re-records its buffer) and the fence. */
	memset(&command_pool_info, 0, sizeof(command_pool_info));
	command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool_info.queueFamilyIndex = run->family;
	run->failed_call = "vkCreateCommandPool";
	status = vkCreateCommandPool(run->device, &command_pool_info, NULL, &run->command_pool);
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
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	run->failed_call = "vkCreateFence";
	status = vkCreateFence(run->device, &fence_info, NULL, &run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: ready to record. */
	return VK_SUCCESS;
}

/* Creates a buffer in memory of the given type, maps it whole and keeps it mapped. */
static VkResult
create_buffer(
	struct vkcoherency *run,
	VkBufferUsageFlags usage,
	uint32_t type,
	VkDeviceSize size,
	struct mapped_buffer *mapped)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate_info;
	void *words;
	VkResult status;

	/* The buffer. */
	memset(mapped, 0, sizeof(*mapped));
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = size;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	run->failed_call = "vkCreateBuffer";
	status = vkCreateBuffer(run->device, &buffer_info, NULL, &mapped->buffer);
	if (status != VK_SUCCESS)
		return status;

	/* Its memory of that type, bound and mapped. */
	vkGetBufferMemoryRequirements(run->device, mapped->buffer, &requirements);
	run->failed_call = "memory type";
	if ((requirements.memoryTypeBits & (1U << type)) == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &mapped->memory);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindBufferMemory";
	status = vkBindBufferMemory(run->device, mapped->buffer, mapped->memory, 0);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkMapMemory";
	status = vkMapMemory(run->device, mapped->memory, 0, VK_WHOLE_SIZE, 0, &words);
	if (status != VK_SUCCESS)
		return status;
	mapped->words = words;
	mapped->size = requirements.size;
	mapped->coherent = (run->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

	/* Succeeded: the buffer is mapped. */
	return VK_SUCCESS;
}

/* Unmaps and destroys a buffer and its memory. */
static void
destroy_buffer(
	struct vkcoherency *run,
	struct mapped_buffer *mapped)
{
	/* In reverse order; null handles are ignored. */
	if (mapped->words != NULL)
		vkUnmapMemory(run->device, mapped->memory);
	vkDestroyBuffer(run->device, mapped->buffer, NULL);
	vkFreeMemory(run->device, mapped->memory, NULL);
	memset(mapped, 0, sizeof(*mapped));
}

/* Flushes the CPU's writes (flush) or invalidates for its reads, only on memory that is not coherent. */
static VkResult
flush_or_invalidate(
	struct vkcoherency *run,
	const struct mapped_buffer *mapped,
	int flush)
{
	VkMappedMemoryRange range;

	/* Coherent memory needs neither. */
	if (mapped->coherent)
		return VK_SUCCESS;

	/* The whole mapping. */
	memset(&range, 0, sizeof(range));
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = mapped->memory;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	if (flush) {
		run->failed_call = "vkFlushMappedMemoryRanges";
		return vkFlushMappedMemoryRanges(run->device, 1, &range);
	}

	run->failed_call = "vkInvalidateMappedMemoryRanges";
	return vkInvalidateMappedMemoryRanges(run->device, 1, &range);
}

/* Ends the recording, submits it with the fence, waits, and resets the fence and the buffer. */
static VkResult
submit_and_wait(
	struct vkcoherency *run)
{
	VkSubmitInfo submit;
	VkResult status;

	/* Ends and submits. */
	run->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(run->command);
	if (status != VK_SUCCESS)
		return status;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &run->command;
	run->failed_call = "vkQueueSubmit";
	status = vkQueueSubmit(run->queue, 1, &submit, run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Waits, then resets for the next round. */
	run->failed_call = "vkWaitForFences";
	status = vkWaitForFences(run->device, 1, &run->fence, VK_TRUE, VKCOHERENCY_WAIT_NS);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkResetFences";
	status = vkResetFences(run->device, 1, &run->fence);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkResetCommandBuffer";
	return vkResetCommandBuffer(run->command, 0);
}

/* The CPU's pattern of a round's input word. */
static uint32_t
pattern(
	uint32_t round,
	uint32_t word)
{
	uint32_t value;

	/* A mix of both, different every round. */
	value = (round + 1U) * 0x9e3779b1U;
	value ^= word * 0x85ebca77U;
	value ^= value >> 15;
	return value;
}

/* Counts one bad word and remembers the first. */
static void
note(
	struct tally *tally,
	uint32_t round,
	uint32_t word,
	uint32_t seen,
	uint32_t wanted,
	int stale)
{
	/* The first bad word of the part. */
	if (!tally->failed) {
		tally->failed = 1;
		tally->first_round = round;
		tally->first_word = word;
		tally->first_seen = seen;
		tally->first_wanted = wanted;
	}

	/* Stale or wrong. */
	if (stale)
		tally->stale++;
	else
		tally->wrong++;
}

/* The buffer part on one memory type. */
static VkResult
run_buffer_part(
	struct vkcoherency *run,
	uint32_t type,
	struct tally *tally)
{
	struct mapped_buffer input;
	struct mapped_buffer output;
	VkDescriptorBufferInfo infos[2];
	VkWriteDescriptorSet writes[2];
	VkCommandBufferBeginInfo begin;
	VkMemoryBarrier barrier;
	uint32_t round;
	uint32_t word;
	uint32_t wanted;
	uint32_t fill;
	uint32_t seen;
	uint32_t previous;
	uint32_t i;
	int stale;
	VkResult status;

	/* The input and the output, bound to the set. */
	memset(&output, 0, sizeof(output));
	status = create_buffer(run, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, type, VKCOHERENCY_WORDS * sizeof(uint32_t), &input);
	if (status == VK_SUCCESS)
		status = create_buffer(run, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, type, VKCOHERENCY_WORDS * sizeof(uint32_t), &output);
	if (status == VK_SUCCESS) {
		memset(infos, 0, sizeof(infos));
		memset(writes, 0, sizeof(writes));
		infos[0].buffer = input.buffer;
		infos[1].buffer = output.buffer;
		for (i = 0; i < 2; i++) {
			infos[i].range = VK_WHOLE_SIZE;
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = run->set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}

		vkUpdateDescriptorSets(run->device, 2, writes, 0, NULL);
	}

	/* Each round: a new input, another fill of the output, the dispatch, the comparison. */
	for (round = 0; status == VK_SUCCESS && round < VKCOHERENCY_BUFFER_ROUNDS; round++) {
		fill = 0xdead0000U ^ round;
		for (word = 0; word < VKCOHERENCY_WORDS; word++) {
			input.words[word] = pattern(round, word);
			output.words[word] = fill;
		}

		status = flush_or_invalidate(run, &input, 1);
		if (status == VK_SUCCESS)
			status = flush_or_invalidate(run, &output, 1);

		/* The dispatch, then the barrier that makes its writes visible to the host. */
		if (status == VK_SUCCESS) {
			memset(&begin, 0, sizeof(begin));
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			run->failed_call = "vkBeginCommandBuffer";
			status = vkBeginCommandBuffer(run->command, &begin);
		}

		if (status == VK_SUCCESS) {
			vkCmdBindPipeline(run->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline);
			vkCmdBindDescriptorSets(run->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline_layout, 0, 1, &run->set, 0, NULL);
			vkCmdDispatch(run->command, VKCOHERENCY_GROUPS, 1, 1);
			memset(&barrier, 0, sizeof(barrier));
			barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(run->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
			status = submit_and_wait(run);
		}

		if (status == VK_SUCCESS)
			status = flush_or_invalidate(run, &output, 0);
		if (status != VK_SUCCESS)
			break;

		/* Every word: right, stale (the fill or the last round's value) or wrong. */
		tally->rounds++;
		for (word = 0; word < VKCOHERENCY_WORDS; word++) {
			wanted = pattern(round, word) + word * 3U;
			seen = output.words[word];
			if (seen == wanted)
				continue;

			/* Stale: the CPU's fill, or what the round before wrote. */
			stale = 0;
			previous = 0;
			if (round > 0)
				previous = pattern(round - 1U, word) + word * 3U;
			if (seen == fill) {
				stale = 1;
			} else if (round > 0 && seen == previous) {
				stale = 1;
			}

			note(tally, round, word, seen, wanted, stale);
		}
	}

	/* The buffers go. */
	destroy_buffer(run, &input);
	destroy_buffer(run, &output);

	/* Reports the call that failed. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: every round ran (the tally says what each saw). */
	return VK_SUCCESS;
}

/* The image part: a clear of an OPTIMAL image to a new colour each round, copied out and compared. */
static VkResult
run_image_part(
	struct vkcoherency *run,
	struct tally *tally)
{
	VkImageCreateInfo image_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate_info;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier image_barrier;
	VkMemoryBarrier barrier;
	VkClearColorValue colour;
	VkImageSubresourceRange range;
	VkBufferImageCopy region;
	struct mapped_buffer readback;
	uint32_t round;
	uint32_t pixel;
	uint32_t wanted;
	uint32_t previous;
	uint32_t fill;
	uint32_t seen;
	uint32_t type;
	uint32_t i;
	int stale;
	VkResult status;

	/* The image in device-local memory if there is any, any type it accepts otherwise. */
	memset(&readback, 0, sizeof(readback));
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	image_info.extent.width = VKCOHERENCY_SIDE;
	image_info.extent.height = VKCOHERENCY_SIDE;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	run->failed_call = "vkCreateImage";
	status = vkCreateImage(run->device, &image_info, NULL, &run->image);
	if (status != VK_SUCCESS)
		return status;
	vkGetImageMemoryRequirements(run->device, run->image, &requirements);
	for (type = 0; type < run->memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) != 0 &&
		    (run->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
			break;
	}

	for (i = 0; type == run->memory.memoryTypeCount && i < run->memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1U << i)) != 0)
			type = i;
	}

	run->failed_call = "image memory type";
	if (type == run->memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &run->image_memory);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindImageMemory";
	status = vkBindImageMemory(run->device, run->image, run->image_memory, 0);
	if (status != VK_SUCCESS)
		return status;

	/* The readback buffer in the first host-visible type it accepts. */
	status = VK_ERROR_FEATURE_NOT_PRESENT;
	for (type = 0; status != VK_SUCCESS && type < run->memory.memoryTypeCount; type++) {
		if ((run->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
			continue;
		destroy_buffer(run, &readback);
		status = create_buffer(run, VK_BUFFER_USAGE_TRANSFER_DST_BIT, type, VKCOHERENCY_PIXELS * 4U, &readback);
	}

	/* Each round: the CPU's fill, the clear, the copy out, the comparison. */
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	previous = 0;
	for (round = 0; status == VK_SUCCESS && round < VKCOHERENCY_IMAGE_ROUNDS; round++) {
		fill = 0xdead0000U ^ round;
		for (pixel = 0; pixel < VKCOHERENCY_PIXELS; pixel++)
			readback.words[pixel] = fill;
		status = flush_or_invalidate(run, &readback, 1);

		/* A colour of whole bytes, so the unorm values come back exact: R round, G 255 - round, B round * 7, A 255. */
		wanted = (round & 0xffU) | ((255U - (round & 0xffU)) << 8) | (((round * 7U) & 0xffU) << 16) | 0xff000000U;
		memset(&colour, 0, sizeof(colour));
		colour.float32[0] = (float)(wanted & 0xffU) / 255.0f;
		colour.float32[1] = (float)((wanted >> 8) & 0xffU) / 255.0f;
		colour.float32[2] = (float)((wanted >> 16) & 0xffU) / 255.0f;
		colour.float32[3] = 1.0f;

		/* Records UNDEFINED -> TRANSFER_DST, the clear, -> TRANSFER_SRC, the copy, the host barrier. */
		if (status == VK_SUCCESS) {
			memset(&begin, 0, sizeof(begin));
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			run->failed_call = "vkBeginCommandBuffer";
			status = vkBeginCommandBuffer(run->command, &begin);
		}

		if (status != VK_SUCCESS)
			break;
		memset(&image_barrier, 0, sizeof(image_barrier));
		image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_barrier.srcAccessMask = 0;
		image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_barrier.image = run->image;
		image_barrier.subresourceRange = range;
		vkCmdPipelineBarrier(run->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image_barrier);
		vkCmdClearColorImage(run->command, run->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
		image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		vkCmdPipelineBarrier(run->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image_barrier);
		memset(&region, 0, sizeof(region));
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = VKCOHERENCY_SIDE;
		region.imageExtent.height = VKCOHERENCY_SIDE;
		region.imageExtent.depth = 1;
		vkCmdCopyImageToBuffer(run->command, run->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(run->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
		status = submit_and_wait(run);
		if (status == VK_SUCCESS)
			status = flush_or_invalidate(run, &readback, 0);
		if (status != VK_SUCCESS)
			break;

		/* Every pixel: right, stale (the fill or the last round's colour) or wrong. */
		tally->rounds++;
		for (pixel = 0; pixel < VKCOHERENCY_PIXELS; pixel++) {
			seen = readback.words[pixel];
			if (seen == wanted)
				continue;

			/* Stale: the CPU's fill, or the round before's colour. */
			stale = 0;
			if (seen == fill) {
				stale = 1;
			} else if (round > 0 && seen == previous) {
				stale = 1;
			}

			note(tally, round, pixel, seen, wanted, stale);
		}

		previous = wanted;
	}

	/* The readback buffer goes; the image goes with the run. */
	destroy_buffer(run, &readback);

	/* Reports the call that failed. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: every round ran (the tally says what each saw). */
	return VK_SUCCESS;
}

/* Prints a part's line. */
static void
print_tally(
	const struct vkcoherency *run,
	const char *part,
	const struct tally *tally)
{
	/* The counts, and the first bad word when there is one. */
	if (!tally->failed) {
		printf("vkcoherency: %s: %s: %u rounds, 0 stale, 0 wrong\n", run->name, part, tally->rounds);
		return;
	}

	printf("vkcoherency: %s: %s: %u rounds, %u stale, %u wrong, first at round %u word %u: 0x%08x, expected 0x%08x\n",
	       run->name, part, tally->rounds, tally->stale, tally->wrong,
	       tally->first_round, tally->first_word, tally->first_seen, tally->first_wanted);
}

/* Destroys every object the run created, newest first. */
static void
destroy(
	struct vkcoherency *run)
{
	/* Leaves the instance alone when no device was created. */
	if (run->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(run->device);
		vkDestroyImage(run->device, run->image, NULL);
		vkFreeMemory(run->device, run->image_memory, NULL);
		vkDestroyFence(run->device, run->fence, NULL);
		vkDestroyCommandPool(run->device, run->command_pool, NULL);
		vkDestroyDescriptorPool(run->device, run->pool, NULL);
		vkDestroyPipeline(run->device, run->pipeline, NULL);
		vkDestroyPipelineLayout(run->device, run->pipeline_layout, NULL);
		vkDestroyDescriptorSetLayout(run->device, run->set_layout, NULL);
		vkDestroyShaderModule(run->device, run->module, NULL);
		vkDestroyDevice(run->device, NULL);
	}

	/* The instance last. */
	if (run->instance != VK_NULL_HANDLE)
		vkDestroyInstance(run->instance, NULL);
}
