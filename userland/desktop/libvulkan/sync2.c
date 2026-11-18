/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Translates the VK_KHR_synchronization2 commands into their Vulkan 1.0 forms.
 *
 * The renderer runs every submission to completion before it answers, and
 * barriers only order work it already orders, so a 1.0 barrier that covers
 * at least the stages and accesses the application named keeps the meaning.
 * Stage and access bits that Vulkan 1.0 has no name for are widened to
 * ALL_COMMANDS and to MEMORY_READ | MEMORY_WRITE.
 */

#include <stdlib.h>
#include <string.h>
#include "internal.h"

/* The stage bits Vulkan 1.0 defines, TOP_OF_PIPE through ALL_COMMANDS. */
#define SYNC2_CORE_STAGES 0x1ffffULL

/* The access bits Vulkan 1.0 defines, INDIRECT_COMMAND_READ through MEMORY_WRITE. */
#define SYNC2_CORE_ACCESSES 0x1ffffULL

/* How many barriers of one kind a single translated 1.0 command carries. */
#define SYNC2_BARRIER_CHUNK 16U

static VkPipelineStageFlags sync2_stages(VkPipelineStageFlags2 stages, VkBool32 source);
static VkAccessFlags sync2_accesses(VkAccessFlags2 accesses);
static void sync2_dependency_stages(const VkDependencyInfo *info, VkPipelineStageFlags2 *source, VkPipelineStageFlags2 *destination);
static void sync2_emit(VkCommandBuffer command, const VkEvent *event, const VkDependencyInfo *info);

/*
 * Records a synchronization2 pipeline barrier as Vulkan 1.0 pipeline barriers.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2KHR(
	VkCommandBuffer commandBuffer,
	const VkDependencyInfo *pDependencyInfo)
{
	/* Emits the barriers with no event to wait for. */
	sync2_emit(commandBuffer, NULL, pDependencyInfo);

	/* Succeeded: the barriers are recorded or the command buffer holds the failure. */
	return;
}

/*
 * Records a synchronization2 event signal as a Vulkan 1.0 event signal.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdSetEvent2KHR(
	VkCommandBuffer commandBuffer,
	VkEvent event,
	const VkDependencyInfo *pDependencyInfo)
{
	VkPipelineStageFlags2 source;
	VkPipelineStageFlags2 destination;
	VkPipelineStageFlags stages;

	/* The event is signaled after every stage any of its barriers waits on. */
	sync2_dependency_stages(pDependencyInfo, &source, &destination);
	stages = sync2_stages(source, VK_TRUE);

	/* Records the 1.0 signal with those stages. */
	vkCmdSetEvent(commandBuffer, event, stages);

	/* Succeeded: the signal is recorded or the command buffer holds the failure. */
	return;
}

/*
 * Records a synchronization2 event reset as a Vulkan 1.0 event reset.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdResetEvent2KHR(
	VkCommandBuffer commandBuffer,
	VkEvent event,
	VkPipelineStageFlags2 stageMask)
{
	VkPipelineStageFlags stages;

	/* The reset happens after the named stages, which are a source scope. */
	stages = sync2_stages(stageMask, VK_TRUE);

	/* Records the 1.0 reset with those stages. */
	vkCmdResetEvent(commandBuffer, event, stages);

	/* Succeeded: the reset is recorded or the command buffer holds the failure. */
	return;
}

/*
 * Records a synchronization2 event wait as one Vulkan 1.0 wait for each event.
 *
 * Each event carries a dependency of its own in synchronization2, and a 1.0
 * wait has one pair of stage masks for all of its events, so every event is
 * waited for separately with its own barriers.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdWaitEvents2KHR(
	VkCommandBuffer commandBuffer,
	uint32_t eventCount,
	const VkEvent *pEvents,
	const VkDependencyInfo *pDependencyInfos)
{
	uint32_t index;

	/* Waits for every event with the dependency that belongs to it. */
	for (index = 0; index < eventCount; index++)
		sync2_emit(commandBuffer, &pEvents[index], &pDependencyInfos[index]);

	/* Succeeded: the waits are recorded or the command buffer holds the failure. */
	return;
}

/*
 * Records a synchronization2 timestamp as a Vulkan 1.0 timestamp.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp2KHR(
	VkCommandBuffer commandBuffer,
	VkPipelineStageFlags2 stage,
	VkQueryPool queryPool,
	uint32_t query)
{
	VkPipelineStageFlags stages;

	/*
	 * The timestamp is written after the named stage.  A stage Vulkan 1.0
	 * cannot name becomes ALL_COMMANDS, which is still one stage.
	 */
	stages = sync2_stages(stage, VK_TRUE);
	if ((stage & ~SYNC2_CORE_STAGES) != 0)
		stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	/* Records the 1.0 timestamp at that stage. */
	vkCmdWriteTimestamp(commandBuffer, (VkPipelineStageFlagBits)stages, queryPool, query);

	/* Succeeded: the timestamp is recorded or the command buffer holds the failure. */
	return;
}

/*
 * Submits synchronization2 batches as Vulkan 1.0 batches.
 *
 * The device has no timeline semaphores and no device groups, so a
 * semaphore's value and a command buffer's device mask carry nothing the
 * 1.0 submission could lose.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2KHR(
	VkQueue queue,
	uint32_t submitCount,
	const VkSubmitInfo2 *pSubmits,
	VkFence fence)
{
	VkSubmitInfo *infos;
	VkSemaphore *semaphores;
	VkPipelineStageFlags *stages;
	VkCommandBuffer *commands;
	size_t semaphore_count;
	size_t command_count;
	size_t bytes;
	uint8_t *block;
	uint32_t index;
	uint32_t item;
	VkResult status;

	/* An empty submission only signals the fence. */
	if (submitCount == 0) {
		status = vkQueueSubmit(queue, 0, NULL, fence);
		if (status != VK_SUCCESS)
			return status;

		/* Succeeded: the fence signal is queued. */
		return VK_SUCCESS;
	}

	/* Counts the semaphores and command buffers every batch names. */
	semaphore_count = 0;
	command_count = 0;
	for (index = 0; index < submitCount; index++) {
		semaphore_count += pSubmits[index].waitSemaphoreInfoCount;
		semaphore_count += pSubmits[index].signalSemaphoreInfoCount;
		command_count += pSubmits[index].commandBufferInfoCount;
	}

	/*
	 * Holds the 1.0 batches and their arrays in one block: the batches, the
	 * semaphores, the command buffers, then one wait stage per semaphore.
	 * The 32-bit stages come last so every 64-bit array stays aligned.
	 */
	bytes = (size_t)submitCount * sizeof(*infos);
	bytes += semaphore_count * sizeof(*semaphores);
	bytes += command_count * sizeof(*commands);
	bytes += semaphore_count * sizeof(*stages);
	block = malloc(bytes);
	if (block == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Carves the block into its four arrays. */
	infos = (VkSubmitInfo *)block;
	semaphores = (VkSemaphore *)(block + (size_t)submitCount * sizeof(*infos));
	commands = (VkCommandBuffer *)(semaphores + semaphore_count);
	stages = (VkPipelineStageFlags *)(commands + command_count);

	/* Translates every batch into the arrays it owns within the block. */
	for (index = 0; index < submitCount; index++) {
		/* The 1.0 batch starts with no arrays and gains them below. */
		memset(&infos[index], 0, sizeof(infos[index]));
		infos[index].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		/* The batch waits for each semaphore before the stages that semaphore names. */
		infos[index].waitSemaphoreCount = pSubmits[index].waitSemaphoreInfoCount;
		infos[index].pWaitSemaphores = semaphores;
		infos[index].pWaitDstStageMask = stages;
		for (item = 0; item < pSubmits[index].waitSemaphoreInfoCount; item++) {
			semaphores[item] = pSubmits[index].pWaitSemaphoreInfos[item].semaphore;
			stages[item] = sync2_stages(pSubmits[index].pWaitSemaphoreInfos[item].stageMask, VK_FALSE);
		}

		/* Moves past the wait arrays this batch used. */
		semaphores += pSubmits[index].waitSemaphoreInfoCount;
		stages += pSubmits[index].waitSemaphoreInfoCount;

		/* The batch runs its command buffers in the order given. */
		infos[index].commandBufferCount = pSubmits[index].commandBufferInfoCount;
		infos[index].pCommandBuffers = commands;
		for (item = 0; item < pSubmits[index].commandBufferInfoCount; item++)
			commands[item] = pSubmits[index].pCommandBufferInfos[item].commandBuffer;

		/* Moves past the command buffers this batch used. */
		commands += pSubmits[index].commandBufferInfoCount;

		/*
		 * The batch signals each semaphore when it completes.  A 1.0
		 * signal has no stage, and the stage of a whole batch is all of it.
		 */
		infos[index].signalSemaphoreCount = pSubmits[index].signalSemaphoreInfoCount;
		infos[index].pSignalSemaphores = semaphores;
		for (item = 0; item < pSubmits[index].signalSemaphoreInfoCount; item++)
			semaphores[item] = pSubmits[index].pSignalSemaphoreInfos[item].semaphore;

		/* Moves past the signal array this batch used. */
		semaphores += pSubmits[index].signalSemaphoreInfoCount;
		stages += pSubmits[index].signalSemaphoreInfoCount;
	}

	/* Submits the translated batches as one 1.0 submission. */
	status = vkQueueSubmit(queue, submitCount, infos, fence);
	free(block);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: every batch is queued with its semaphores and the fence. */
	return VK_SUCCESS;
}

/* Translates synchronization2 stages into the 1.0 stages that cover them. */
static VkPipelineStageFlags
sync2_stages(
	VkPipelineStageFlags2 stages,
	VkBool32 source)
{
	VkPipelineStageFlags core;

	/*
	 * No stage at all is a scope Vulkan 1.0 writes as the pipeline's first
	 * stage on the source side and its last stage on the destination side.
	 */
	if (stages == 0) {
		/* The source scope that waits for nothing. */
		if (source)
			return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

		/* The destination scope that blocks nothing. */
		return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	}

	/* Keeps every stage Vulkan 1.0 names exactly. */
	core = (VkPipelineStageFlags)(stages & SYNC2_CORE_STAGES);

	/* A stage Vulkan 1.0 cannot name, such as video decode or copy, is covered by all commands. */
	if ((stages & ~SYNC2_CORE_STAGES) != 0)
		core |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	/* Succeeded: the 1.0 stages cover the named stages. */
	return core;
}

/* Translates synchronization2 accesses into the 1.0 accesses that cover them. */
static VkAccessFlags
sync2_accesses(
	VkAccessFlags2 accesses)
{
	VkAccessFlags core;

	/* Keeps every access Vulkan 1.0 names exactly. */
	core = (VkAccessFlags)(accesses & SYNC2_CORE_ACCESSES);

	/* An access Vulkan 1.0 cannot name, such as a video decode read, is covered by all memory access. */
	if ((accesses & ~SYNC2_CORE_ACCESSES) != 0)
		core |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

	/* Succeeded: the 1.0 accesses cover the named accesses. */
	return core;
}

/* Collects the source and destination stages of every barrier in one dependency. */
static void
sync2_dependency_stages(
	const VkDependencyInfo *info,
	VkPipelineStageFlags2 *source,
	VkPipelineStageFlags2 *destination)
{
	uint32_t index;

	/* A dependency with no barrier orders no stage. */
	*source = 0;
	*destination = 0;

	/* Gathers the stages of the global memory barriers. */
	for (index = 0; index < info->memoryBarrierCount; index++) {
		*source |= info->pMemoryBarriers[index].srcStageMask;
		*destination |= info->pMemoryBarriers[index].dstStageMask;
	}

	/* Gathers the stages of the buffer barriers. */
	for (index = 0; index < info->bufferMemoryBarrierCount; index++) {
		*source |= info->pBufferMemoryBarriers[index].srcStageMask;
		*destination |= info->pBufferMemoryBarriers[index].dstStageMask;
	}

	/* Gathers the stages of the image barriers. */
	for (index = 0; index < info->imageMemoryBarrierCount; index++) {
		*source |= info->pImageMemoryBarriers[index].srcStageMask;
		*destination |= info->pImageMemoryBarriers[index].dstStageMask;
	}

	/* Succeeded: both masks hold every stage of the dependency. */
	return;
}

/*
 * Records one dependency as 1.0 barriers, waiting for an event when one is given.
 *
 * A 1.0 command has one pair of stage masks, so each translated command
 * uses the union of the dependency's stages.  The barriers go out in chunks
 * of a fixed size, which keeps the translation free of allocation; several
 * 1.0 barriers with the same stages order the same work as one.
 */
static void
sync2_emit(
	VkCommandBuffer command,
	const VkEvent *event,
	const VkDependencyInfo *info)
{
	VkMemoryBarrier memory[SYNC2_BARRIER_CHUNK];
	VkBufferMemoryBarrier buffer[SYNC2_BARRIER_CHUNK];
	VkImageMemoryBarrier image[SYNC2_BARRIER_CHUNK];
	const VkMemoryBarrier2 *memory2;
	const VkBufferMemoryBarrier2 *buffer2;
	const VkImageMemoryBarrier2 *image2;
	VkPipelineStageFlags2 source2;
	VkPipelineStageFlags2 destination2;
	VkPipelineStageFlags source;
	VkPipelineStageFlags destination;
	uint32_t memory_next;
	uint32_t buffer_next;
	uint32_t image_next;
	uint32_t memory_count;
	uint32_t buffer_count;
	uint32_t image_count;
	VkBool32 first;

	/* Every translated command carries the union of the dependency's stages. */
	sync2_dependency_stages(info, &source2, &destination2);
	source = sync2_stages(source2, VK_TRUE);
	destination = sync2_stages(destination2, VK_FALSE);

	/*
	 * Emits chunks until every barrier is out.  A dependency with no barrier
	 * still emits one command, which is its execution dependency.
	 */
	memory_next = 0;
	buffer_next = 0;
	image_next = 0;
	first = VK_TRUE;
	while (first ||
	       memory_next < info->memoryBarrierCount ||
	       buffer_next < info->bufferMemoryBarrierCount ||
	       image_next < info->imageMemoryBarrierCount) {
		/* Translates the next chunk of global memory barriers. */
		memory_count = 0;
		while (memory_count < SYNC2_BARRIER_CHUNK && memory_next < info->memoryBarrierCount) {
			memory2 = &info->pMemoryBarriers[memory_next];
			memset(&memory[memory_count], 0, sizeof(memory[memory_count]));
			memory[memory_count].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memory[memory_count].srcAccessMask = sync2_accesses(memory2->srcAccessMask);
			memory[memory_count].dstAccessMask = sync2_accesses(memory2->dstAccessMask);
			memory_count++;
			memory_next++;
		}

		/* Translates the next chunk of buffer barriers, ownership transfer included. */
		buffer_count = 0;
		while (buffer_count < SYNC2_BARRIER_CHUNK && buffer_next < info->bufferMemoryBarrierCount) {
			buffer2 = &info->pBufferMemoryBarriers[buffer_next];
			memset(&buffer[buffer_count], 0, sizeof(buffer[buffer_count]));
			buffer[buffer_count].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			buffer[buffer_count].srcAccessMask = sync2_accesses(buffer2->srcAccessMask);
			buffer[buffer_count].dstAccessMask = sync2_accesses(buffer2->dstAccessMask);
			buffer[buffer_count].srcQueueFamilyIndex = buffer2->srcQueueFamilyIndex;
			buffer[buffer_count].dstQueueFamilyIndex = buffer2->dstQueueFamilyIndex;
			buffer[buffer_count].buffer = buffer2->buffer;
			buffer[buffer_count].offset = buffer2->offset;
			buffer[buffer_count].size = buffer2->size;
			buffer_count++;
			buffer_next++;
		}

		/* Translates the next chunk of image barriers, layout transitions included. */
		image_count = 0;
		while (image_count < SYNC2_BARRIER_CHUNK && image_next < info->imageMemoryBarrierCount) {
			image2 = &info->pImageMemoryBarriers[image_next];
			memset(&image[image_count], 0, sizeof(image[image_count]));
			image[image_count].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			image[image_count].srcAccessMask = sync2_accesses(image2->srcAccessMask);
			image[image_count].dstAccessMask = sync2_accesses(image2->dstAccessMask);
			image[image_count].oldLayout = image2->oldLayout;
			image[image_count].newLayout = image2->newLayout;
			image[image_count].srcQueueFamilyIndex = image2->srcQueueFamilyIndex;
			image[image_count].dstQueueFamilyIndex = image2->dstQueueFamilyIndex;
			image[image_count].image = image2->image;
			image[image_count].subresourceRange = image2->subresourceRange;
			image_count++;
			image_next++;
		}

		/* Records the chunk as a wait for the event, or as a plain barrier. */
		if (event != NULL) {
			vkCmdWaitEvents(
				command,
				1,
				event,
				source,
				destination,
				memory_count,
				memory,
				buffer_count,
				buffer,
				image_count,
				image);
		} else {
			vkCmdPipelineBarrier(
				command,
				source,
				destination,
				info->dependencyFlags,
				memory_count,
				memory,
				buffer_count,
				buffer,
				image_count,
				image);
		}

		/* The first command is out; later ones only carry remaining barriers. */
		first = VK_FALSE;
	}

	/* Succeeded: every barrier is recorded or the command buffer holds the failure. */
	return;
}
