/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan 1.2 core: the feature and property structures (what the
 * library offers of them, the check of those a device creation asks for
 * and the chain of features the renderer is told of), and the commands of
 * 1.2 the library makes from those of 1.0: render passes of 1.2's form,
 * imageless framebuffers (resources.c), the host's query reset, timeline
 * semaphores and the buffer device addresses it does not offer yet.  The
 * indirect draws with a count go to the renderer (commands.c).  A native
 * driver's events, which the library keeps, are held back with the same
 * batches as the timelines.
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sync-internal.h"

/* The stage and access bits Vulkan 1.0 defines, TOP_OF_PIPE through ALL_COMMANDS and INDIRECT_COMMAND_READ through MEMORY_WRITE. */
#define RENDER_PASS2_CORE_STAGES	0x1ffffULL
#define RENDER_PASS2_CORE_ACCESSES	0x1ffffULL

/*
 * The index of a field among the VkBool32 fields of
 * VkPhysicalDeviceVulkan12Features, which follow one another from
 * samplerMirrorClampToEdge to subgroupBroadcastDynamicId.
 */
#define CORE12_INDEX(field)		((uint32_t)((offsetof(VkPhysicalDeviceVulkan12Features, field) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32)))

/*
 * The most a timeline semaphore's value may differ from the values its
 * pending signals and waits name: the library keeps every value whole, so
 * any difference.
 */
#define CORE12_TIMELINE_DIFFERENCE	UINT64_MAX

/*
 * The longest one wait of vkWaitSemaphores sleeps before it looks again
 * at the timelines it waits for, in nanoseconds: a host signal wakes it at
 * once, the completion of a batch only when it next looks.
 */
#define CORE12_WAIT_SLICE_NS		UINT64_C(2000000)

/*
 * A fence of the library's that a batch signaling timeline semaphores was
 * followed by: it signals when the batch completes.  Each pending signal
 * of the batch holds a reference, and so does a wait that sleeps on it.
 */
struct core12_fence {
	struct VkDevice_T *device;
	VkFence fence;
	uint32_t references;
	VkBool32 signaled;
};

/* A value a submitted batch will give a timeline semaphore when it completes. */
struct core12_signal {
	struct core12_signal *next;
	uint64_t value;
	struct VkQueue_T *queue;
	struct core12_fence *fence;
};

/*
 * A timeline semaphore: the value it has reached and the values submitted
 * batches will give it.  It is the library's alone; the renderer's
 * semaphore of the same handle is never waited for or signaled.
 */
struct vulkan_timeline {
	struct VkDevice_T *device;
	uint64_t value;
	struct core12_signal *signals;
};

/* A set or reset of an event by a submitted batch, which takes effect when the batch completes. */
struct core12_event_change {
	struct core12_event_change *next;
	VkBool32 set;
	struct VkQueue_T *queue;
	struct core12_fence *fence;
};

/*
 * An event of a native driver, the library's alone: whether it is set (by
 * the host, or by batches that completed) and the changes submitted
 * batches will make, oldest first.  The renderer has no event; a wait for
 * it is a barrier in the recording, and the batch that holds the wait
 * goes to the renderer once the event is set or will be by then.
 */
struct vulkan_event {
	VkBool32 set;
	struct core12_event_change *changes;
};

/*
 * One batch held back because it waits for a value no batch has been
 * submitted to signal yet: a copy of it and of its arrays, and the fence of
 * its submission when it is that submission's last batch.  Batches of one
 * queue go to the renderer in the order they were submitted.
 */
struct core12_deferred {
	struct core12_deferred *next;
	struct VkQueue_T *queue;
	VkSubmitInfo submit;
	VkTimelineSemaphoreSubmitInfo timeline;
	VkFence fence;
	void *storage;
};

/* How a batch can go to the renderer: now, after a fence of another queue's batch, or not yet. */
enum core12_ready {
	CORE12_READY,
	CORE12_AFTER_FENCE,
	CORE12_NOT_YET
};

/*
 * One Vulkan 1.2 feature structure of its own (one of the extensions 1.2
 * promoted): its type, where its VkBool32 fields start in it, and the run
 * of VkPhysicalDeviceVulkan12Features fields they are, in the same order.
 */
struct core12_record {
	VkStructureType type;
	size_t offset;
	uint32_t first;
	uint32_t count;
};

/*
 * The Vulkan 1.0 form of a VkRenderPassCreateInfo2 that
 * render_pass2_convert makes: the 1.0 record, the multiview record chained
 * to it when a subpass has views, and the arrays both point to, owned
 * until render_pass2_release.  resolves says a subpass resolves depth or
 * stencil, which the 1.0 form cannot carry.
 */
struct render_pass2_form {
	VkRenderPassCreateInfo info;
	VkRenderPassMultiviewCreateInfo multiview;
	VkAttachmentDescription *attachments;
	VkSubpassDescription *subpasses;
	VkAttachmentReference *references;
	VkSubpassDependency *dependencies;
	uint32_t *view_masks;
	int32_t *view_offsets;
	VkBool32 resolves;
};

/*
 * The Vulkan 1.2 feature structures of their own and where their fields
 * are among those of VkPhysicalDeviceVulkan12Features.
 *
 * Values referenced: the Vulkan 1.2 specification (section 39.1, the
 * feature structures of the promoted extensions and
 * VkPhysicalDeviceVulkan12Features).
 */
static const struct core12_record core12_records[] = {
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, offsetof(VkPhysicalDevice8BitStorageFeatures, storageBuffer8BitAccess), CORE12_INDEX(storageBuffer8BitAccess), 3U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, offsetof(VkPhysicalDeviceShaderAtomicInt64Features, shaderBufferInt64Atomics), CORE12_INDEX(shaderBufferInt64Atomics), 2U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16), CORE12_INDEX(shaderFloat16), 2U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, offsetof(VkPhysicalDeviceDescriptorIndexingFeatures, shaderInputAttachmentArrayDynamicIndexing), CORE12_INDEX(shaderInputAttachmentArrayDynamicIndexing), 20U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, offsetof(VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout), CORE12_INDEX(scalarBlockLayout), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, offsetof(VkPhysicalDeviceImagelessFramebufferFeatures, imagelessFramebuffer), CORE12_INDEX(imagelessFramebuffer), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, offsetof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, uniformBufferStandardLayout), CORE12_INDEX(uniformBufferStandardLayout), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, offsetof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes), CORE12_INDEX(shaderSubgroupExtendedTypes), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, offsetof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, separateDepthStencilLayouts), CORE12_INDEX(separateDepthStencilLayouts), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, offsetof(VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset), CORE12_INDEX(hostQueryReset), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, offsetof(VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore), CORE12_INDEX(timelineSemaphore), 1U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, offsetof(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress), CORE12_INDEX(bufferDeviceAddress), 3U},
	{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, offsetof(VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModel), CORE12_INDEX(vulkanMemoryModel), 3U}
};

/*
 * The state of every timeline semaphore and the batches held back.  The
 * mutex protects the timelines' values and signals, the held-back batches
 * and the flush's flags; it is held while a batch goes to the renderer and
 * never while the library sleeps on a fence.  The condition is broadcast
 * whenever a timeline's value or signals change.  They live as long as the
 * library.
 */
static pthread_mutex_t core12_timeline_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t core12_timeline_cond = PTHREAD_COND_INITIALIZER;

/* The held-back batches, oldest first. */
static struct core12_deferred *core12_deferred_head;

/*
 * Whether a thread is sending held-back batches now, and whether something
 * changed while it did, so it looks once more before it stops.
 */
static VkBool32 core12_flushing;
static VkBool32 core12_flush_again;

static void core12_offered(struct VkPhysicalDevice_T *physical, VkPhysicalDeviceVulkan12Features *offered);
static const struct core12_record *core12_record_find(VkStructureType type);
static VkBool32 *core12_record_flag(void *record, const struct core12_record *entry, uint32_t index);
static VkResult core12_check_vulkan11(const struct vulkan_core11_facts *facts, const VkPhysicalDeviceVulkan11Features *features);
static VkResult core12_check_flag(VkBool32 requested, VkBool32 reported);
static struct VkQueue_T *core12_query_queue(struct VkDevice_T *device);
static struct vulkan_timeline *core12_timeline_of(VkSemaphore semaphore);
static void core12_fence_release(struct core12_fence *fence);
static VkResult core12_fence_check(struct core12_fence *fence);
static VkResult core12_refresh(struct vulkan_timeline *timeline);
static struct vulkan_event *core12_event_of(uint64_t handle);
static VkResult core12_event_refresh(struct vulkan_event *event);
static VkBool32 core12_event_earlier(const VkSubmitInfo *submit, uint32_t buffer, uint32_t action, uint64_t handle, VkBool32 *set);
static VkResult core12_events_ready(struct VkQueue_T *queue, const VkSubmitInfo *submit, enum core12_ready *ready, struct core12_fence **fence);
static VkBool32 core12_batch_changes(const VkSubmitInfo *submit);
static VkResult core12_changes_add(struct VkQueue_T *queue, const VkSubmitInfo *submit, struct core12_fence *fence);
static VkBool32 core12_queue_held(struct VkQueue_T *queue);
static const VkTimelineSemaphoreSubmitInfo *core12_batch_values(const VkSubmitInfo *submit);
static VkResult core12_batch_ready(struct VkQueue_T *queue, const VkSubmitInfo *submit, enum core12_ready *ready, struct core12_fence **fence);
static VkResult core12_batch_send(struct VkQueue_T *queue, const VkSubmitInfo *submit, VkFence fence);
static VkResult core12_submit_from(struct VkQueue_T *queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, VkBool32 held);
static VkResult core12_defer(struct VkQueue_T *queue, const VkSubmitInfo *submit, VkFence fence);
static void core12_flush(void);
static VkResult core12_wait_reached(const VkSemaphoreWaitInfo *info, VkBool32 *reached, struct core12_fence **fence);
static void core12_sleep(uint64_t nanoseconds);
static VkResult render_pass2_convert(const VkRenderPassCreateInfo2 *info, struct render_pass2_form *form);
static void render_pass2_release(struct render_pass2_form *form);
static VkAttachmentReference *render_pass2_references(const VkAttachmentReference2 *references, uint32_t count, VkAttachmentReference *storage);
static void render_pass2_dependency(const VkSubpassDependency2 *dependency, VkSubpassDependency *converted);
static VkPipelineStageFlags render_pass2_stages(VkPipelineStageFlags2 stages, VkBool32 source);
static void render_pass2_encode_reference(struct vulkan_writer *writer, const VkAttachmentReference2 *reference);
static void render_pass2_encode_references(struct vulkan_writer *writer, const VkAttachmentReference2 *references, uint32_t count);

/*
 * Creates a render pass of Vulkan 1.2's form.  The library gives it the
 * renderer in its Vulkan 1.0 form (views in VkRenderPassMultiviewCreateInfo,
 * the separate stencil layouts in the layout of both aspects), except a
 * render pass that resolves depth or stencil, which 1.0 cannot carry: that
 * one goes as it is to a renderer that reported Vulkan 1.2.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass2(
	VkDevice device,
	const VkRenderPassCreateInfo2 *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkRenderPass *pRenderPass)
{
	struct VkDevice_T *owner;
	struct render_pass2_form form;
	VkResult status;

	/* The 1.0 form, which the library keeps of every render pass. */
	owner = vulkan_device(device);
	status = render_pass2_convert(pCreateInfo, &form);
	if (status != VK_SUCCESS)
		return status;

	/* A depth or stencil resolve needs the renderer's Vulkan 1.2 render pass. */
	if (form.resolves && !owner->physical->core12.reported) {
		render_pass2_release(&form);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	/* Created in the form the renderer needs. */
	if (form.resolves)
		status = vulkan_render_pass_create(owner, &form.info, pCreateInfo, pAllocator, pRenderPass);
	else
		status = vulkan_render_pass_create(owner, &form.info, NULL, pAllocator, pRenderPass);
	render_pass2_release(&form);

	/* Refused, or succeeded: the render pass exists. */
	return status;
}

/*
 * Begins a render pass of Vulkan 1.2's form: the same as vkCmdBeginRenderPass
 * with the contents of the first subpass.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderPass2(
	VkCommandBuffer commandBuffer,
	const VkRenderPassBeginInfo *pRenderPassBegin,
	const VkSubpassBeginInfo *pSubpassBeginInfo)
{
	/* The 1.0 command. */
	vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, pSubpassBeginInfo->contents);
}

/* Moves to the next subpass, as vkCmdNextSubpass does. */
VKAPI_ATTR void VKAPI_CALL
vkCmdNextSubpass2(
	VkCommandBuffer commandBuffer,
	const VkSubpassBeginInfo *pSubpassBeginInfo,
	const VkSubpassEndInfo *pSubpassEndInfo)
{
	/* The 1.0 command; the end of a subpass carries nothing the library keeps. */
	(void)pSubpassEndInfo;
	vkCmdNextSubpass(commandBuffer, pSubpassBeginInfo->contents);
}

/* Ends a render pass, as vkCmdEndRenderPass does. */
VKAPI_ATTR void VKAPI_CALL
vkCmdEndRenderPass2(
	VkCommandBuffer commandBuffer,
	const VkSubpassEndInfo *pSubpassEndInfo)
{
	/* The 1.0 command. */
	(void)pSubpassEndInfo;
	vkCmdEndRenderPass(commandBuffer);
}

/*
 * Resets queries from the host: the library records vkCmdResetQueryPool in
 * a command buffer of its own and runs it to completion on a queue that
 * can reset queries, so every renderer resets them the one way it knows.
 *
 * Procedure referenced: the Vulkan 1.2 specification (section 17.2,
 * vkResetQueryPool: the same effect as vkCmdResetQueryPool executed on a
 * queue; the queries must not be in use).
 */
VKAPI_ATTR void VKAPI_CALL
vkResetQueryPool(
	VkDevice device,
	VkQueryPool queryPool,
	uint32_t firstQuery,
	uint32_t queryCount)
{
	struct VkDevice_T *owner;
	struct VkQueue_T *queue;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo buffer_info;
	VkCommandBufferBeginInfo begin_info;
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkResult status;

	/* A queue of a family that runs graphics or compute work. */
	owner = vulkan_device(device);
	queue = core12_query_queue(owner);
	if (queue == NULL || queryCount == 0U)
		return;

	/* A pool of that family and one command buffer from it. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool_info.queueFamilyIndex = queue->family;
	status = vkCreateCommandPool(device, &pool_info, NULL, &pool);
	if (status != VK_SUCCESS)
		return;
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	buffer_info.commandPool = pool;
	buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	buffer_info.commandBufferCount = 1U;
	status = vkAllocateCommandBuffers(device, &buffer_info, &command);

	/* The reset, recorded. */
	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (status == VK_SUCCESS)
		status = vkBeginCommandBuffer(command, &begin_info);
	if (status == VK_SUCCESS) {
		vkCmdResetQueryPool(command, queryPool, firstQuery, queryCount);
		status = vkEndCommandBuffer(command);
	}

	/* Run to completion. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence = VK_NULL_HANDLE;
	if (status == VK_SUCCESS)
		status = vkCreateFence(device, &fence_info, NULL, &fence);
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &command;
	if (status == VK_SUCCESS)
		status = vkQueueSubmit((VkQueue)queue, 1U, &submit, fence);
	if (status == VK_SUCCESS)
		(void)vkWaitForFences(device, 1U, &fence, VK_TRUE, UINT64_MAX);

	/* The library's own objects. */
	vkDestroyFence(device, fence, NULL);
	vkDestroyCommandPool(device, pool, NULL);
}

/* Gives no address: the library does not offer bufferDeviceAddress yet, so no buffer has one. */
VKAPI_ATTR VkDeviceAddress VKAPI_CALL
vkGetBufferDeviceAddress(
	VkDevice device,
	const VkBufferDeviceAddressInfo *pInfo)
{
	/* The application may not ask without the feature. */
	(void)device;
	(void)pInfo;
	return 0U;
}

/* Gives no capture address: the library does not offer bufferDeviceAddressCaptureReplay. */
VKAPI_ATTR uint64_t VKAPI_CALL
vkGetBufferOpaqueCaptureAddress(
	VkDevice device,
	const VkBufferDeviceAddressInfo *pInfo)
{
	/* The application may not ask without the feature. */
	(void)device;
	(void)pInfo;
	return 0U;
}

/* Gives no capture address: the library does not offer bufferDeviceAddressCaptureReplay. */
VKAPI_ATTR uint64_t VKAPI_CALL
vkGetDeviceMemoryOpaqueCaptureAddress(
	VkDevice device,
	const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
	/* The application may not ask without the feature. */
	(void)device;
	(void)pInfo;
	return 0U;
}

/*
 * Makes a semaphore a timeline semaphore when its creation chains a
 * VkSemaphoreTypeCreateInfo of VK_SEMAPHORE_TYPE_TIMELINE, with the value
 * named there; a binary semaphore is left as it is.
 */
VkResult
vulkan_timeline_create(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	const VkSemaphoreCreateInfo *info)
{
	const VkBaseInStructure *next;
	const VkSemaphoreTypeCreateInfo *type;
	struct vulkan_timeline *timeline;

	/* The type the creation names, binary when it names none. */
	sync->timeline = NULL;
	type = NULL;
	for (next = info->pNext; next != NULL; next = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
			type = (const VkSemaphoreTypeCreateInfo *)next;
	}
	if (type == NULL || type->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE)
		return VK_SUCCESS;

	/* The timeline at its first value. */
	timeline = calloc(1, sizeof(*timeline));
	if (timeline == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	timeline->device = device;
	timeline->value = type->initialValue;
	sync->timeline = timeline;

	/* Succeeded: the semaphore is a timeline. */
	return VK_SUCCESS;
}

/* Forgets a timeline semaphore's state when the semaphore is destroyed. */
void
vulkan_timeline_destroy(
	struct vulkan_sync *sync)
{
	struct vulkan_timeline *timeline;
	struct core12_signal *signal;

	/* A binary semaphore has none. */
	if (sync == NULL || sync->timeline == NULL)
		return;

	/* Its signals (the application waited for them all before destroying it). */
	pthread_mutex_lock(&core12_timeline_mutex);
	timeline = sync->timeline;
	sync->timeline = NULL;
	while (timeline->signals != NULL) {
		signal = timeline->signals;
		timeline->signals = signal->next;
		core12_fence_release(signal->fence);
		free(signal);
	}
	pthread_mutex_unlock(&core12_timeline_mutex);
	free(timeline);
}

/*
 * Tells whether a submission goes through the library: it waits for or
 * signals a timeline semaphore, a command buffer of it acts on a native
 * driver's event, or batches of its queue are held back (it goes after
 * them).
 */
VkBool32
vulkan_library_sync_named(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits)
{
	struct VkCommandBuffer_T *command;
	VkBool32 held;
	uint32_t index;
	uint32_t item;

	/* Every semaphore and command buffer of every batch. */
	for (index = 0; index < count; index++) {
		for (item = 0; item < submits[index].commandBufferCount; item++) {
			command = (struct VkCommandBuffer_T *)submits[index].pCommandBuffers[item];
			if (command != NULL && command->event_count != 0U)
				return VK_TRUE;
		}
		for (item = 0; item < submits[index].waitSemaphoreCount; item++) {
			if (core12_timeline_of(submits[index].pWaitSemaphores[item]) != NULL)
				return VK_TRUE;
		}
		for (item = 0; item < submits[index].signalSemaphoreCount; item++) {
			if (core12_timeline_of(submits[index].pSignalSemaphores[item]) != NULL)
				return VK_TRUE;
		}
	}

	/*
	 * Binary semaphores only: through the library still when the queue's
	 * batches are held back.  Only a submission to the queue holds one
	 * back, and the application keeps the queue to this thread.
	 */
	pthread_mutex_lock(&core12_timeline_mutex);
	held = core12_queue_held(queue);
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Succeeded: held back batches, or the renderer's alone. */
	return held;
}

/*
 * Submits batches that wait for or signal timeline semaphores, or act on
 * a native driver's events.  The renderer knows binary semaphores only,
 * so the library sends each batch
 * without its timeline semaphores once the values it waits for are
 * reached, or will be by a batch sent before it on the same queue (a queue
 * runs its batches in order), or by a batch of another queue, whose
 * completion it then waits for; a batch that waits for a value nothing has
 * been submitted to signal yet is held back, with every later batch of its
 * queue, until a host signal or a submission signals it.  A batch that
 * signals a timeline is followed by a fence of the library's, whose
 * completion gives the timeline its value.  Events are alike: a batch
 * that waits for an event goes once the event is set, or will be by a
 * batch sent before it on the same queue (or by an earlier command of the
 * batch), or after the batch of another queue that sets it, and is held
 * back otherwise until the host sets it; a batch that sets or resets
 * events is followed by a fence of the library's, whose completion
 * changes them.
 *
 * XXX A held-back batch waits from its start: commands recorded before
 * the wait also wait for the host's set.
 *
 * Procedure referenced: the Vulkan 1.2 specification (section 7.4.2,
 * Semaphore Waiting: a wait may be submitted before the signal it waits
 * for; section 7.4.1, Semaphore Signaling: the value is set when the batch
 * completes; section 7.5, Events: an event set by the host or by an
 * earlier command in submission order releases vkCmdWaitEvents, whose
 * dependency covers the stages and barriers it names).
 */
VkResult
vulkan_library_sync_submit(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	VkFence fence)
{
	VkResult status;

	/* After held-back batches of the same queue, held back too; otherwise sent in turn. */
	pthread_mutex_lock(&core12_timeline_mutex);
	status = core12_submit_from(queue, count, submits, fence, core12_queue_held(queue));

	/* What was held back may go now. */
	if (status == VK_SUCCESS)
		core12_flush();
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Refused, or succeeded: every batch is sent or held back. */
	return status;
}

/*
 * Gives the value a timeline semaphore has reached.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetSemaphoreCounterValue(
	VkDevice device,
	VkSemaphore semaphore,
	uint64_t *pValue)
{
	struct vulkan_timeline *timeline;
	VkResult status;

	/* The value, after the batches that completed since it was last read. */
	(void)device;
	timeline = core12_timeline_of(semaphore);
	if (timeline == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	pthread_mutex_lock(&core12_timeline_mutex);
	status = core12_refresh(timeline);
	*pValue = timeline->value;
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Refused (a device lost), or succeeded: the value is written. */
	return status;
}

/*
 * Waits on the host until timeline semaphores reach values: all of them,
 * or any one with VK_SEMAPHORE_WAIT_ANY_BIT, or until the timeout passes.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkWaitSemaphores(
	VkDevice device,
	const VkSemaphoreWaitInfo *pWaitInfo,
	uint64_t timeout)
{
	struct core12_fence *fence;
	uint64_t now;
	uint64_t deadline;
	uint64_t slice;
	VkBool32 reached;
	VkResult status;

	/* When the wait gives up. */
	status = vulkan_sync_clock(&now);
	if (status != VK_SUCCESS)
		return status;
	deadline = UINT64_MAX;
	if (timeout < UINT64_MAX - now)
		deadline = now + timeout;

	/* Looks, then sleeps on a batch's fence or a host signal, until reached or out of time. */
	pthread_mutex_lock(&core12_timeline_mutex);
	for (;;) {
		status = core12_wait_reached(pWaitInfo, &reached, &fence);
		if (status != VK_SUCCESS || reached)
			break;
		status = vulkan_sync_clock(&now);
		if (status != VK_SUCCESS)
			break;
		if (now >= deadline) {
			status = VK_TIMEOUT;
			break;
		}
		slice = deadline - now;
		if (slice > CORE12_WAIT_SLICE_NS)
			slice = CORE12_WAIT_SLICE_NS;

		/* A batch's fence that would give one value, or a host signal. */
		if (fence != NULL) {
			fence->references++;
			pthread_mutex_unlock(&core12_timeline_mutex);
			(void)vkWaitForFences(device, 1U, &fence->fence, VK_TRUE, slice);
			pthread_mutex_lock(&core12_timeline_mutex);
			core12_fence_release(fence);
		} else {
			core12_sleep(slice);
		}
	}
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Refused, timed out, or succeeded: the values are reached. */
	return status;
}

/*
 * Signals a timeline semaphore from the host: it reaches the value at
 * once, and batches held back for it go to the renderer.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkSignalSemaphore(
	VkDevice device,
	const VkSemaphoreSignalInfo *pSignalInfo)
{
	struct vulkan_timeline *timeline;

	/* The semaphore, a timeline. */
	(void)device;
	timeline = core12_timeline_of(pSignalInfo->semaphore);
	if (timeline == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The value, the waiters told, the held-back batches sent. */
	pthread_mutex_lock(&core12_timeline_mutex);
	if (pSignalInfo->value > timeline->value)
		timeline->value = pSignalInfo->value;
	pthread_cond_broadcast(&core12_timeline_cond);
	core12_flush();
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Succeeded: the semaphore has the value. */
	return VK_SUCCESS;
}

/* Makes a native driver's event the library's, reset. */
VkResult
vulkan_event_create(
	struct vulkan_sync *sync)
{
	struct vulkan_event *event;

	/* Reset, no change pending. */
	event = calloc(1, sizeof(*event));
	if (event == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	sync->library_event = event;

	/* Succeeded: the event exists. */
	return VK_SUCCESS;
}

/* Forgets a native driver's event when it is destroyed. */
void
vulkan_event_destroy(
	struct vulkan_sync *sync)
{
	struct vulkan_event *event;
	struct core12_event_change *change;

	/* None (a Venus host's event). */
	if (sync == NULL || sync->library_event == NULL)
		return;

	/* Its changes (the application waited for their batches before destroying it). */
	pthread_mutex_lock(&core12_timeline_mutex);
	event = sync->library_event;
	sync->library_event = NULL;
	while (event->changes != NULL) {
		change = event->changes;
		event->changes = change->next;
		core12_fence_release(change->fence);
		free(change);
	}
	pthread_mutex_unlock(&core12_timeline_mutex);
	free(event);
}

/* Gives a native driver's event's state: VK_EVENT_SET or VK_EVENT_RESET, or a failure. */
VkResult
vulkan_event_status(
	struct vulkan_sync *sync)
{
	struct vulkan_event *event;
	VkResult status;

	/* The state, after the batches that completed since it was last read. */
	event = sync->library_event;
	pthread_mutex_lock(&core12_timeline_mutex);
	status = core12_event_refresh(event);
	if (status == VK_SUCCESS) {
		status = VK_EVENT_RESET;
		if (event->set)
			status = VK_EVENT_SET;
	}
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Refused (a device lost), or succeeded: the state. */
	return status;
}

/*
 * Sets or resets a native driver's event from the host, after the
 * changes of the batches that completed before; batches held back for it
 * go to the renderer once it is set.
 */
VkResult
vulkan_event_host(
	struct vulkan_sync *sync,
	VkBool32 set)
{
	struct vulkan_event *event;
	VkResult status;

	/* The state, the held-back batches sent. */
	event = sync->library_event;
	pthread_mutex_lock(&core12_timeline_mutex);
	status = core12_event_refresh(event);
	if (status == VK_SUCCESS) {
		event->set = set;
		if (set)
			core12_flush();
	}
	pthread_mutex_unlock(&core12_timeline_mutex);

	/* Refused (a device lost), or succeeded: the event has the state. */
	return status;
}

/*
 * Returns one of the VkBool32 fields of VkPhysicalDeviceVulkan12Features
 * by its index in declared order (index below VULKAN_CORE12_FEATURE_COUNT).
 */
VkBool32 *
vulkan_core12_feature(
	VkPhysicalDeviceVulkan12Features *features,
	uint32_t index)
{
	char *first;

	/* The fields follow one another from the first. */
	first = (char *)features + offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge);

	/* Succeeded: the field of that index. */
	return (VkBool32 *)(first + (size_t)index * sizeof(VkBool32));
}

/*
 * Answers the Vulkan 1.2 feature structures chained to
 * VkPhysicalDeviceFeatures2: VkPhysicalDeviceVulkan11Features from the 1.1
 * facts, and VkPhysicalDeviceVulkan12Features and the structures of the
 * promoted extensions from what the library offers (core12_offered).
 */
void
vulkan_core12_features(
	struct VkPhysicalDevice_T *physical,
	VkBaseOutStructure *chain)
{
	const struct vulkan_core11_facts *facts;
	const struct core12_record *entry;
	VkPhysicalDeviceVulkan12Features offered;
	VkPhysicalDeviceVulkan12Features *core12;
	VkPhysicalDeviceVulkan11Features *core11;
	VkBaseOutStructure *next;
	uint32_t index;

	/* What the device offers of Vulkan 1.2. */
	facts = &physical->core11;
	core12_offered(physical, &offered);

	/* Each structure wherever the caller chained it; its header is kept. */
	for (next = chain; next != NULL; next = next->pNext) {

		/* The 1.1 features gathered in one structure. */
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
			core11 = (VkPhysicalDeviceVulkan11Features *)next;
			core11->storageBuffer16BitAccess = facts->storage_buffer_16bit;
			core11->uniformAndStorageBuffer16BitAccess = facts->uniform_storage_buffer_16bit;
			core11->storagePushConstant16 = facts->storage_push_constant_16;
			core11->storageInputOutput16 = facts->storage_input_output_16;
			core11->multiview = facts->multiview;
			core11->multiviewGeometryShader = facts->multiview_geometry_shader;
			core11->multiviewTessellationShader = facts->multiview_tessellation_shader;
			core11->variablePointersStorageBuffer = facts->variable_pointers_storage_buffer;
			core11->variablePointers = facts->variable_pointers;
			core11->protectedMemory = VK_FALSE;
			core11->samplerYcbcrConversion = VK_FALSE;
			core11->shaderDrawParameters = facts->shader_draw_parameters;
			continue;
		}

		/* The 1.2 features gathered in one structure. */
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
			core12 = (VkPhysicalDeviceVulkan12Features *)next;
			for (index = 0U; index < VULKAN_CORE12_FEATURE_COUNT; index++)
				*vulkan_core12_feature(core12, index) = *vulkan_core12_feature(&offered, index);
			continue;
		}

		/* A structure of a promoted extension: its run of the 1.2 features. */
		entry = core12_record_find((VkStructureType)next->sType);
		if (entry == NULL)
			continue;
		for (index = 0U; index < entry->count; index++)
			*core12_record_flag(next, entry, index) = *vulkan_core12_feature(&offered, entry->first + index);
	}

	/* Succeeded: the structures the library knows are answered. */
	return;
}

/*
 * Answers the Vulkan 1.2 property structures chained to
 * VkPhysicalDeviceProperties2 from what the renderer reported, except
 * what the library decides: no conformance is claimed, the min and max
 * filter is not offered, and a timeline's values may differ by any amount.
 * The identity of VkPhysicalDeviceVulkan11Properties is the caller's
 * (external-properties.c).
 */
void
vulkan_core12_properties(
	struct VkPhysicalDevice_T *physical,
	VkBaseOutStructure *chain)
{
	const struct vulkan_core11_facts *facts;
	VkPhysicalDeviceVulkan12Properties offered;
	VkPhysicalDeviceVulkan11Properties *core11;
	VkPhysicalDeviceVulkan12Properties *core12;
	VkPhysicalDeviceDriverProperties *driver;
	VkPhysicalDeviceFloatControlsProperties *controls;
	VkPhysicalDeviceDescriptorIndexingProperties *indexing;
	VkPhysicalDeviceDepthStencilResolveProperties *resolve;
	VkPhysicalDeviceSamplerFilterMinmaxProperties *minmax;
	VkPhysicalDeviceTimelineSemaphoreProperties *timeline;
	VkBaseOutStructure *next;
	void *link;

	/* The renderer's properties with the library's own decisions. */
	facts = &physical->core11;
	offered = physical->core12.properties;
	memset(&offered.conformanceVersion, 0, sizeof(offered.conformanceVersion));
	offered.filterMinmaxSingleComponentFormats = VK_FALSE;
	offered.filterMinmaxImageComponentMapping = VK_FALSE;
	offered.maxTimelineSemaphoreValueDifference = CORE12_TIMELINE_DIFFERENCE;

	/* Each structure wherever the caller chained it; its header is kept. */
	for (next = chain; next != NULL; next = next->pNext) {

		/* The fields of a structure the library knows. */
		switch (next->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
			core11 = (VkPhysicalDeviceVulkan11Properties *)next;
			core11->subgroupSize = facts->subgroup_size;
			core11->subgroupSupportedStages = facts->subgroup_stages;
			core11->subgroupSupportedOperations = facts->subgroup_operations;
			core11->subgroupQuadOperationsInAllStages = facts->subgroup_quad_all_stages;
			core11->pointClippingBehavior = (VkPointClippingBehavior)facts->point_clipping;
			core11->maxMultiviewViewCount = facts->max_multiview_view_count;
			core11->maxMultiviewInstanceIndex = facts->max_multiview_instance_index;
			core11->protectedNoFault = VK_FALSE;
			core11->maxPerSetDescriptors = facts->max_per_set_descriptors;
			core11->maxMemoryAllocationSize = facts->max_memory_allocation_size;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
			core12 = (VkPhysicalDeviceVulkan12Properties *)next;
			link = core12->pNext;
			*core12 = offered;
			core12->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
			core12->pNext = link;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
			driver = (VkPhysicalDeviceDriverProperties *)next;
			driver->driverID = offered.driverID;
			memcpy(driver->driverName, offered.driverName, sizeof(driver->driverName));
			memcpy(driver->driverInfo, offered.driverInfo, sizeof(driver->driverInfo));
			driver->conformanceVersion = offered.conformanceVersion;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES:
			/* The same fields in the same order, from the denormal independence to RTZ of 64 bits. */
			controls = (VkPhysicalDeviceFloatControlsProperties *)next;
			memcpy(&controls->denormBehaviorIndependence, &offered.denormBehaviorIndependence,
			    offsetof(VkPhysicalDeviceFloatControlsProperties, shaderRoundingModeRTZFloat64) + sizeof(VkBool32) - offsetof(VkPhysicalDeviceFloatControlsProperties, denormBehaviorIndependence));
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
			/* The same fields in the same order, from the pools' total to the sets' input attachments. */
			indexing = (VkPhysicalDeviceDescriptorIndexingProperties *)next;
			memcpy(&indexing->maxUpdateAfterBindDescriptorsInAllPools, &offered.maxUpdateAfterBindDescriptorsInAllPools,
			    offsetof(VkPhysicalDeviceDescriptorIndexingProperties, maxDescriptorSetUpdateAfterBindInputAttachments) + sizeof(uint32_t) - offsetof(VkPhysicalDeviceDescriptorIndexingProperties, maxUpdateAfterBindDescriptorsInAllPools));
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:
			resolve = (VkPhysicalDeviceDepthStencilResolveProperties *)next;
			resolve->supportedDepthResolveModes = offered.supportedDepthResolveModes;
			resolve->supportedStencilResolveModes = offered.supportedStencilResolveModes;
			resolve->independentResolveNone = offered.independentResolveNone;
			resolve->independentResolve = offered.independentResolve;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES:
			minmax = (VkPhysicalDeviceSamplerFilterMinmaxProperties *)next;
			minmax->filterMinmaxSingleComponentFormats = offered.filterMinmaxSingleComponentFormats;
			minmax->filterMinmaxImageComponentMapping = offered.filterMinmaxImageComponentMapping;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
			timeline = (VkPhysicalDeviceTimelineSemaphoreProperties *)next;
			timeline->maxTimelineSemaphoreValueDifference = offered.maxTimelineSemaphoreValueDifference;
			break;
		default:
			break;
		}
	}

	/* Succeeded: the structures the library knows are answered. */
	return;
}

/*
 * Checks the Vulkan 1.2 feature records chained to a device creation:
 * every feature asked for in VkPhysicalDeviceVulkan11Features,
 * VkPhysicalDeviceVulkan12Features or the structures of the promoted
 * extensions being one the device offers.
 */
VkResult
vulkan_core12_device_check(
	struct VkPhysicalDevice_T *physical,
	const VkDeviceCreateInfo *info)
{
	const struct core12_record *entry;
	const VkBaseInStructure *next;
	VkPhysicalDeviceVulkan12Features offered;
	VkPhysicalDeviceVulkan12Features *requested;
	VkResult status;
	uint32_t index;

	/* What the device offers of Vulkan 1.2. */
	core12_offered(physical, &offered);

	/* Each record against it. */
	status = VK_SUCCESS;
	for (next = info->pNext; next != NULL && status == VK_SUCCESS; next = next->pNext) {

		/* The 1.1 features gathered in one structure. */
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
			status = core12_check_vulkan11(&physical->core11, (const VkPhysicalDeviceVulkan11Features *)next);
			continue;
		}

		/* The 1.2 features gathered in one structure. */
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
			requested = (VkPhysicalDeviceVulkan12Features *)next;
			for (index = 0U; index < VULKAN_CORE12_FEATURE_COUNT && status == VK_SUCCESS; index++)
				status = core12_check_flag(*vulkan_core12_feature(requested, index), *vulkan_core12_feature(&offered, index));
			continue;
		}

		/* A structure of a promoted extension. */
		entry = core12_record_find((VkStructureType)next->sType);
		if (entry == NULL)
			continue;
		for (index = 0U; index < entry->count && status == VK_SUCCESS; index++)
			status = core12_check_flag(*core12_record_flag((void *)next, entry, index), *vulkan_core12_feature(&offered, entry->first + index));
	}

	/* Refused, or succeeded: the device can be made with what was asked. */
	return status;
}

/*
 * Makes the chain of feature records the renderer is told of at a device
 * creation, in storage: the Vulkan 1.1 feature structures the caller asked
 * for (one by one, or gathered in VkPhysicalDeviceVulkan11Features), and,
 * to a renderer that reported Vulkan 1.2, one
 * VkPhysicalDeviceVulkan12Features with the features its shaders and
 * commands carry.  The features the library makes itself are not the
 * renderer's and stay false in it.  Returns the chain's first record.
 */
const void *
vulkan_core12_device_chain(
	struct VkPhysicalDevice_T *physical,
	const VkDeviceCreateInfo *info,
	struct vulkan_device_chain *storage)
{
	const struct core12_record *entry;
	const VkBaseInStructure *next;
	const VkPhysicalDeviceVulkan11Features *core11;
	VkPhysicalDeviceVulkan12Features requested;
	VkBaseOutStructure *chain;
	VkBool32 wanted;
	uint32_t index;

	/* Nothing asked for yet. */
	memset(storage, 0, sizeof(*storage));
	memset(&requested, 0, sizeof(requested));
	storage->multiview.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
	storage->storage.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
	storage->pointers.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
	storage->parameters.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
	storage->core12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

	/* What each record asks for, gathered. */
	for (next = info->pNext; next != NULL; next = next->pNext) {
		switch (next->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
			core11 = (const VkPhysicalDeviceVulkan11Features *)next;
			storage->storage.storageBuffer16BitAccess |= core11->storageBuffer16BitAccess;
			storage->storage.uniformAndStorageBuffer16BitAccess |= core11->uniformAndStorageBuffer16BitAccess;
			storage->storage.storagePushConstant16 |= core11->storagePushConstant16;
			storage->storage.storageInputOutput16 |= core11->storageInputOutput16;
			storage->multiview.multiview |= core11->multiview;
			storage->multiview.multiviewGeometryShader |= core11->multiviewGeometryShader;
			storage->multiview.multiviewTessellationShader |= core11->multiviewTessellationShader;
			storage->pointers.variablePointersStorageBuffer |= core11->variablePointersStorageBuffer;
			storage->pointers.variablePointers |= core11->variablePointers;
			storage->parameters.shaderDrawParameters |= core11->shaderDrawParameters;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
			storage->multiview.multiview |= ((const VkPhysicalDeviceMultiviewFeatures *)next)->multiview;
			storage->multiview.multiviewGeometryShader |= ((const VkPhysicalDeviceMultiviewFeatures *)next)->multiviewGeometryShader;
			storage->multiview.multiviewTessellationShader |= ((const VkPhysicalDeviceMultiviewFeatures *)next)->multiviewTessellationShader;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
			storage->storage.storageBuffer16BitAccess |= ((const VkPhysicalDevice16BitStorageFeatures *)next)->storageBuffer16BitAccess;
			storage->storage.uniformAndStorageBuffer16BitAccess |= ((const VkPhysicalDevice16BitStorageFeatures *)next)->uniformAndStorageBuffer16BitAccess;
			storage->storage.storagePushConstant16 |= ((const VkPhysicalDevice16BitStorageFeatures *)next)->storagePushConstant16;
			storage->storage.storageInputOutput16 |= ((const VkPhysicalDevice16BitStorageFeatures *)next)->storageInputOutput16;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
			storage->pointers.variablePointersStorageBuffer |= ((const VkPhysicalDeviceVariablePointersFeatures *)next)->variablePointersStorageBuffer;
			storage->pointers.variablePointers |= ((const VkPhysicalDeviceVariablePointersFeatures *)next)->variablePointers;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
			storage->parameters.shaderDrawParameters |= ((const VkPhysicalDeviceShaderDrawParametersFeatures *)next)->shaderDrawParameters;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
			for (index = 0U; index < VULKAN_CORE12_FEATURE_COUNT; index++)
				*vulkan_core12_feature(&requested, index) |= *vulkan_core12_feature((VkPhysicalDeviceVulkan12Features *)next, index);
			break;
		default:
			entry = core12_record_find((VkStructureType)next->sType);
			if (entry == NULL)
				break;
			for (index = 0U; index < entry->count; index++)
				*vulkan_core12_feature(&requested, entry->first + index) |= *core12_record_flag((void *)next, entry, index);
			break;
		}
	}

	/* The 1.1 records the renderer reads, those asked for (protected memory and YCbCr never are). */
	chain = NULL;
	if (storage->parameters.shaderDrawParameters) {
		storage->parameters.pNext = chain;
		chain = (VkBaseOutStructure *)&storage->parameters;
	}
	if (storage->pointers.variablePointersStorageBuffer || storage->pointers.variablePointers) {
		storage->pointers.pNext = chain;
		chain = (VkBaseOutStructure *)&storage->pointers;
	}
	if (storage->storage.storageBuffer16BitAccess || storage->storage.uniformAndStorageBuffer16BitAccess ||
	    storage->storage.storagePushConstant16 || storage->storage.storageInputOutput16) {
		storage->storage.pNext = chain;
		chain = (VkBaseOutStructure *)&storage->storage;
	}
	if (storage->multiview.multiview) {
		storage->multiview.pNext = chain;
		chain = (VkBaseOutStructure *)&storage->multiview;
	}

	/* The 1.2 record, to a renderer that reported Vulkan 1.2: the renderer's features asked for. */
	if (!physical->core12.reported)
		return chain;
	for (index = 0U; index < VULKAN_CORE12_FEATURE_COUNT; index++) {
		wanted = *vulkan_core12_feature(&requested, index) && *vulkan_core12_feature(&physical->core12.features, index);
		*vulkan_core12_feature(&storage->core12, index) = wanted ? VK_TRUE : VK_FALSE;
	}
	storage->core12.imagelessFramebuffer = VK_FALSE;
	storage->core12.hostQueryReset = VK_FALSE;
	storage->core12.timelineSemaphore = VK_FALSE;
	storage->core12.pNext = chain;

	/* Succeeded: the chain is made. */
	return &storage->core12;
}

/*
 * Encodes a VkRenderPassCreateInfo2 as Venus does: each record's sType, its
 * chain and its fields, an array as its length and its elements, a pointer
 * as its presence and its record.  Of the chains only a subpass's
 * VkSubpassDescriptionDepthStencilResolve is sent; the layouts are those of
 * Vulkan 1.0 and a dependency's masks those of its 1.0 form (converted).
 *
 * Procedure referenced: venus-protocol docs/VK_EXT_command_serialization.txt
 * (the encoding of records, arrays, pointers and pNext chains).
 */
void
vulkan_render_pass2_encode(
	struct vulkan_writer *writer,
	const VkRenderPassCreateInfo2 *info,
	const VkRenderPassCreateInfo *converted)
{
	const VkAttachmentDescription *attachment;
	const VkSubpassDescription2 *subpass;
	const VkSubpassDescriptionDepthStencilResolve *resolve;
	const VkSubpassDependency *dependency;
	uint32_t index;
	int32_t offset;

	/* The record's header and flags; its own chain is not sent. */
	vulkan_write_u32(writer, info->sType);
	vulkan_write_u64(writer, 0U);
	vulkan_write_u32(writer, info->flags);

	/*
	 * The attachments, of the 1.0 form (the same fields; the stencil's
	 * own layouts in the layout of both aspects).
	 */
	vulkan_write_u32(writer, converted->attachmentCount);
	vulkan_write_u64(writer, converted->pAttachments != NULL ? converted->attachmentCount : 0U);
	for (index = 0; converted->pAttachments != NULL && index < converted->attachmentCount; index++) {
		attachment = &converted->pAttachments[index];
		vulkan_write_u32(writer, VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2);
		vulkan_write_u64(writer, 0U);
		vulkan_write_u32(writer, attachment->flags);
		vulkan_write_u32(writer, attachment->format);
		vulkan_write_u32(writer, attachment->samples);
		vulkan_write_u32(writer, attachment->loadOp);
		vulkan_write_u32(writer, attachment->storeOp);
		vulkan_write_u32(writer, attachment->stencilLoadOp);
		vulkan_write_u32(writer, attachment->stencilStoreOp);
		vulkan_write_u32(writer, vulkan_wire_image_layout(attachment->initialLayout));
		vulkan_write_u32(writer, vulkan_wire_image_layout(attachment->finalLayout));
	}

	/* The subpasses, each with its resolve of depth or stencil chained. */
	vulkan_write_u32(writer, info->subpassCount);
	vulkan_write_u64(writer, info->subpassCount);
	for (index = 0; index < info->subpassCount; index++) {
		subpass = &info->pSubpasses[index];
		vulkan_write_u32(writer, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2);
		resolve = vulkan_render_pass2_resolve(subpass);
		if (resolve != NULL) {
			vulkan_write_u64(writer, 1U);
			vulkan_write_u32(writer, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
			vulkan_write_u64(writer, 0U);
			vulkan_write_u32(writer, resolve->depthResolveMode);
			vulkan_write_u32(writer, resolve->stencilResolveMode);
			vulkan_write_u64(writer, 1U);
			render_pass2_encode_reference(writer, resolve->pDepthStencilResolveAttachment);
		} else {
			vulkan_write_u64(writer, 0U);
		}
		vulkan_write_u32(writer, subpass->flags);
		vulkan_write_u32(writer, subpass->pipelineBindPoint);
		vulkan_write_u32(writer, subpass->viewMask);
		vulkan_write_u32(writer, subpass->inputAttachmentCount);
		render_pass2_encode_references(writer, subpass->pInputAttachments, subpass->inputAttachmentCount);
		vulkan_write_u32(writer, subpass->colorAttachmentCount);
		render_pass2_encode_references(writer, subpass->pColorAttachments, subpass->colorAttachmentCount);
		render_pass2_encode_references(writer, subpass->pResolveAttachments, subpass->colorAttachmentCount);
		vulkan_write_u64(writer, subpass->pDepthStencilAttachment != NULL ? 1U : 0U);
		if (subpass->pDepthStencilAttachment != NULL)
			render_pass2_encode_reference(writer, subpass->pDepthStencilAttachment);
		vulkan_write_u32(writer, subpass->preserveAttachmentCount);
		vulkan_write_u64(writer, subpass->pPreserveAttachments != NULL ? subpass->preserveAttachmentCount : 0U);
		if (subpass->pPreserveAttachments != NULL)
			vulkan_write_bytes(writer, subpass->pPreserveAttachments, (size_t)subpass->preserveAttachmentCount * sizeof(uint32_t));
	}

	/*
	 * The dependencies of the 1.0 form, with the view offsets of the 1.2
	 * record when it has them (a subpass of a split render pass has none).
	 */
	vulkan_write_u32(writer, converted->dependencyCount);
	vulkan_write_u64(writer, converted->pDependencies != NULL ? converted->dependencyCount : 0U);
	for (index = 0; converted->pDependencies != NULL && index < converted->dependencyCount; index++) {
		dependency = &converted->pDependencies[index];
		offset = 0;
		if (info->pDependencies != NULL && index < info->dependencyCount)
			offset = info->pDependencies[index].viewOffset;
		vulkan_write_u32(writer, VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2);
		vulkan_write_u64(writer, 0U);
		vulkan_write_u32(writer, dependency->srcSubpass);
		vulkan_write_u32(writer, dependency->dstSubpass);
		vulkan_write_u32(writer, dependency->srcStageMask);
		vulkan_write_u32(writer, dependency->dstStageMask);
		vulkan_write_u32(writer, dependency->srcAccessMask);
		vulkan_write_u32(writer, dependency->dstAccessMask);
		vulkan_write_u32(writer, dependency->dependencyFlags);
		vulkan_write_u32(writer, (uint32_t)offset);
	}

	/* The correlated view masks. */
	vulkan_write_u32(writer, info->correlatedViewMaskCount);
	vulkan_write_u64(writer, info->pCorrelatedViewMasks != NULL ? info->correlatedViewMaskCount : 0U);
	if (info->pCorrelatedViewMasks != NULL)
		vulkan_write_bytes(writer, info->pCorrelatedViewMasks, (size_t)info->correlatedViewMaskCount * sizeof(uint32_t));
}

/*
 * Gives what the device offers of the Vulkan 1.2 features: the renderer's
 * features that its shaders and commands carry as they are, the four the
 * library makes itself (imageless framebuffers, separate depth and stencil
 * layouts, the host's query reset and timeline semaphores) when the
 * device names Vulkan 1.2, and none of those the library does not pass on
 * yet (descriptor indexing, the min and max filter, buffer device
 * addresses).
 */
static void
core12_offered(
	struct VkPhysicalDevice_T *physical,
	VkPhysicalDeviceVulkan12Features *offered)
{
	const VkPhysicalDeviceVulkan12Features *reported;

	/* Nothing, unless the renderer reported Vulkan 1.2. */
	memset(offered, 0, sizeof(*offered));
	offered->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	if (!physical->core12.reported)
		return;

	/* The sampler's address mode and the indirect count go to the renderer as they are. */
	reported = &physical->core12.features;
	offered->samplerMirrorClampToEdge = reported->samplerMirrorClampToEdge;
	offered->drawIndirectCount = reported->drawIndirectCount;

	/* The shader's types, layouts, memory model, outputs and subgroups are the renderer's compiler's. */
	offered->storageBuffer8BitAccess = reported->storageBuffer8BitAccess;
	offered->uniformAndStorageBuffer8BitAccess = reported->uniformAndStorageBuffer8BitAccess;
	offered->storagePushConstant8 = reported->storagePushConstant8;
	offered->shaderBufferInt64Atomics = reported->shaderBufferInt64Atomics;
	offered->shaderSharedInt64Atomics = reported->shaderSharedInt64Atomics;
	offered->shaderFloat16 = reported->shaderFloat16;
	offered->shaderInt8 = reported->shaderInt8;
	offered->scalarBlockLayout = reported->scalarBlockLayout;
	offered->uniformBufferStandardLayout = reported->uniformBufferStandardLayout;
	offered->shaderSubgroupExtendedTypes = reported->shaderSubgroupExtendedTypes;
	offered->vulkanMemoryModel = reported->vulkanMemoryModel;
	offered->vulkanMemoryModelDeviceScope = reported->vulkanMemoryModelDeviceScope;
	offered->vulkanMemoryModelAvailabilityVisibilityChains = reported->vulkanMemoryModelAvailabilityVisibilityChains;
	offered->shaderOutputViewportIndex = reported->shaderOutputViewportIndex;
	offered->shaderOutputLayer = reported->shaderOutputLayer;
	offered->subgroupBroadcastDynamicId = reported->subgroupBroadcastDynamicId;

	/* The library's own, on a device that names Vulkan 1.2. */
	if (physical->properties.apiVersion < VK_MAKE_VERSION(1, 2, 0))
		return;
	offered->imagelessFramebuffer = VK_TRUE;
	offered->separateDepthStencilLayouts = VK_TRUE;
	offered->hostQueryReset = VK_TRUE;
	offered->timelineSemaphore = VK_TRUE;

	/* Succeeded: the offer is made. */
	return;
}

/* Finds the promoted extension's feature structure of a type, NULL for another type. */
static const struct core12_record *
core12_record_find(
	VkStructureType type)
{
	uint32_t index;

	/* The table, by type. */
	for (index = 0U; index < sizeof(core12_records) / sizeof(core12_records[0]); index++) {
		if (core12_records[index].type == type)
			return &core12_records[index];
	}

	/* Not one of them. */
	return NULL;
}

/* Returns one VkBool32 field of a promoted extension's feature structure by its index. */
static VkBool32 *
core12_record_flag(
	void *record,
	const struct core12_record *entry,
	uint32_t index)
{
	/* The fields follow one another from the entry's offset. */
	return (VkBool32 *)((char *)record + entry->offset + (size_t)index * sizeof(VkBool32));
}

/* Refuses a 1.1 feature asked for in VkPhysicalDeviceVulkan11Features that the device does not report. */
static VkResult
core12_check_vulkan11(
	const struct vulkan_core11_facts *facts,
	const VkPhysicalDeviceVulkan11Features *features)
{
	VkResult status;

	/* Multiview and 16-bit storage. */
	status = core12_check_flag(features->multiview, facts->multiview);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->multiviewGeometryShader, facts->multiview_geometry_shader);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->multiviewTessellationShader, facts->multiview_tessellation_shader);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->storageBuffer16BitAccess, facts->storage_buffer_16bit);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->uniformAndStorageBuffer16BitAccess, facts->uniform_storage_buffer_16bit);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->storagePushConstant16, facts->storage_push_constant_16);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->storageInputOutput16, facts->storage_input_output_16);

	/* Variable pointers, protected memory, YCbCr and draw parameters. */
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->variablePointersStorageBuffer, facts->variable_pointers_storage_buffer);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->variablePointers, facts->variable_pointers);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->protectedMemory, VK_FALSE);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->samplerYcbcrConversion, VK_FALSE);
	if (status == VK_SUCCESS)
		status = core12_check_flag(features->shaderDrawParameters, facts->shader_draw_parameters);

	/* Refused, or succeeded: every feature asked for is reported. */
	return status;
}

/* Refuses a feature asked for that the device does not offer. */
static VkResult
core12_check_flag(
	VkBool32 requested,
	VkBool32 reported)
{
	/* A feature not asked for, or one the device has. */
	if (!requested || reported)
		return VK_SUCCESS;

	/* Refused: the feature is absent. */
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

/*
 * Makes the Vulkan 1.0 form of a VkRenderPassCreateInfo2: the attachments,
 * subpasses and dependencies without their 1.2 records, each subpass's
 * view mask and each dependency's view offset in a
 * VkRenderPassMultiviewCreateInfo when a subpass has views, and the masks
 * of a VkMemoryBarrier2 chained to a dependency in the dependency's own.
 * The separate stencil layouts are left: the layout of both aspects is the
 * renderer's (wire.c: vulkan_wire_image_layout).  The aspects of an input
 * attachment are left too: a depth and stencil input attachment is read
 * whole.
 */
static VkResult
render_pass2_convert(
	const VkRenderPassCreateInfo2 *info,
	struct render_pass2_form *form)
{
	const VkSubpassDescription2 *subpass;
	const VkSubpassDescriptionDepthStencilResolve *resolve;
	VkSubpassDescription *converted;
	VkAttachmentReference *reference;
	VkBool32 views;
	size_t count;
	uint32_t index;

	/* Nothing made yet. */
	memset(form, 0, sizeof(*form));

	/* How many references every subpass needs. */
	count = 0;
	for (index = 0; index < info->subpassCount; index++) {
		subpass = &info->pSubpasses[index];
		count += subpass->inputAttachmentCount + subpass->colorAttachmentCount;
		if (subpass->pResolveAttachments != NULL)
			count += subpass->colorAttachmentCount;
		if (subpass->pDepthStencilAttachment != NULL)
			count++;
	}

	/* The arrays (an array of none is a valid one of one element). */
	form->attachments = calloc(info->attachmentCount + 1U, sizeof(*form->attachments));
	form->subpasses = calloc(info->subpassCount + 1U, sizeof(*form->subpasses));
	form->references = calloc(count + 1U, sizeof(*form->references));
	form->dependencies = calloc(info->dependencyCount + 1U, sizeof(*form->dependencies));
	form->view_masks = calloc(info->subpassCount + 1U, sizeof(*form->view_masks));
	form->view_offsets = calloc(info->dependencyCount + 1U, sizeof(*form->view_offsets));
	if (form->attachments == NULL || form->subpasses == NULL || form->references == NULL ||
	    form->dependencies == NULL || form->view_masks == NULL || form->view_offsets == NULL) {
		render_pass2_release(form);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The attachments without their records. */
	for (index = 0; index < info->attachmentCount; index++) {
		form->attachments[index].flags = info->pAttachments[index].flags;
		form->attachments[index].format = info->pAttachments[index].format;
		form->attachments[index].samples = info->pAttachments[index].samples;
		form->attachments[index].loadOp = info->pAttachments[index].loadOp;
		form->attachments[index].storeOp = info->pAttachments[index].storeOp;
		form->attachments[index].stencilLoadOp = info->pAttachments[index].stencilLoadOp;
		form->attachments[index].stencilStoreOp = info->pAttachments[index].stencilStoreOp;
		form->attachments[index].initialLayout = info->pAttachments[index].initialLayout;
		form->attachments[index].finalLayout = info->pAttachments[index].finalLayout;
	}

	/* The subpasses, their references in the shared array, and their views. */
	reference = form->references;
	views = VK_FALSE;
	for (index = 0; index < info->subpassCount; index++) {
		subpass = &info->pSubpasses[index];
		converted = &form->subpasses[index];
		converted->flags = subpass->flags;
		converted->pipelineBindPoint = subpass->pipelineBindPoint;
		converted->inputAttachmentCount = subpass->inputAttachmentCount;
		converted->pInputAttachments = reference;
		reference = render_pass2_references(subpass->pInputAttachments, subpass->inputAttachmentCount, reference);
		converted->colorAttachmentCount = subpass->colorAttachmentCount;
		converted->pColorAttachments = reference;
		reference = render_pass2_references(subpass->pColorAttachments, subpass->colorAttachmentCount, reference);
		if (subpass->pResolveAttachments != NULL) {
			converted->pResolveAttachments = reference;
			reference = render_pass2_references(subpass->pResolveAttachments, subpass->colorAttachmentCount, reference);
		}
		if (subpass->pDepthStencilAttachment != NULL) {
			converted->pDepthStencilAttachment = reference;
			reference = render_pass2_references(subpass->pDepthStencilAttachment, 1U, reference);
		}
		converted->preserveAttachmentCount = subpass->preserveAttachmentCount;
		converted->pPreserveAttachments = subpass->pPreserveAttachments;
		form->view_masks[index] = subpass->viewMask;
		if (subpass->viewMask != 0U)
			views = VK_TRUE;

		/* A resolve of depth or stencil the 1.0 form cannot carry. */
		resolve = vulkan_render_pass2_resolve(subpass);
		if (resolve != NULL)
			form->resolves = VK_TRUE;
	}

	/* The dependencies and their view offsets. */
	for (index = 0; index < info->dependencyCount; index++) {
		render_pass2_dependency(&info->pDependencies[index], &form->dependencies[index]);
		form->view_offsets[index] = info->pDependencies[index].viewOffset;
	}

	/* The 1.0 record. */
	form->info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	form->info.flags = info->flags;
	form->info.attachmentCount = info->attachmentCount;
	form->info.pAttachments = form->attachments;
	form->info.subpassCount = info->subpassCount;
	form->info.pSubpasses = form->subpasses;
	form->info.dependencyCount = info->dependencyCount;
	form->info.pDependencies = form->dependencies;

	/* The views, when a subpass has them. */
	if (views) {
		form->multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
		form->multiview.subpassCount = info->subpassCount;
		form->multiview.pViewMasks = form->view_masks;
		form->multiview.dependencyCount = info->dependencyCount;
		form->multiview.pViewOffsets = form->view_offsets;
		form->multiview.correlationMaskCount = info->correlatedViewMaskCount;
		form->multiview.pCorrelationMasks = info->pCorrelatedViewMasks;
		form->info.pNext = &form->multiview;
	}

	/* Succeeded: the 1.0 form is made. */
	return VK_SUCCESS;
}

/* Frees the arrays of a render pass's Vulkan 1.0 form. */
static void
render_pass2_release(
	struct render_pass2_form *form)
{
	/* Each array, made or not. */
	free(form->view_offsets);
	free(form->view_masks);
	free(form->dependencies);
	free(form->references);
	free(form->subpasses);
	free(form->attachments);
	memset(form, 0, sizeof(*form));
}

/* Copies attachment references into their 1.0 form at storage and returns the place after them. */
static VkAttachmentReference *
render_pass2_references(
	const VkAttachmentReference2 *references,
	uint32_t count,
	VkAttachmentReference *storage)
{
	uint32_t index;

	/* The attachment and its layout of each. */
	for (index = 0; index < count; index++) {
		storage[index].attachment = references[index].attachment;
		storage[index].layout = references[index].layout;
	}

	/* Succeeded: the next free reference. */
	return storage + count;
}

/*
 * Makes a dependency's 1.0 form: its masks, or those of a VkMemoryBarrier2
 * chained to it, which replace them; a stage or access bit 1.0 has no name
 * for widens the mask to every stage or every memory access.
 *
 * Values referenced: the Vulkan 1.3 specification (section 8.1,
 * VkSubpassDependency2: a chained VkMemoryBarrier2 defines the stage and
 * access masks and the ones of the dependency are ignored).
 */
static void
render_pass2_dependency(
	const VkSubpassDependency2 *dependency,
	VkSubpassDependency *converted)
{
	const VkBaseInStructure *next;
	const VkMemoryBarrier2 *barrier;

	/* The dependency's own fields. */
	converted->srcSubpass = dependency->srcSubpass;
	converted->dstSubpass = dependency->dstSubpass;
	converted->srcStageMask = dependency->srcStageMask;
	converted->dstStageMask = dependency->dstStageMask;
	converted->srcAccessMask = dependency->srcAccessMask;
	converted->dstAccessMask = dependency->dstAccessMask;
	converted->dependencyFlags = dependency->dependencyFlags;

	/* A chained barrier of synchronization2 instead. */
	for (next = dependency->pNext; next != NULL; next = next->pNext) {
		if (next->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER_2)
			continue;
		barrier = (const VkMemoryBarrier2 *)next;
		converted->srcStageMask = render_pass2_stages(barrier->srcStageMask, VK_TRUE);
		converted->dstStageMask = render_pass2_stages(barrier->dstStageMask, VK_FALSE);
		converted->srcAccessMask = (VkAccessFlags)barrier->srcAccessMask;
		if ((barrier->srcAccessMask & ~RENDER_PASS2_CORE_ACCESSES) != 0U)
			converted->srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		converted->dstAccessMask = (VkAccessFlags)barrier->dstAccessMask;
		if ((barrier->dstAccessMask & ~RENDER_PASS2_CORE_ACCESSES) != 0U)
			converted->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	}
}

/* Gives a synchronization2 stage mask as a 1.0 one: none as the end of its scope, unnamed bits as every stage. */
static VkPipelineStageFlags
render_pass2_stages(
	VkPipelineStageFlags2 stages,
	VkBool32 source)
{
	/* No stage: the top of the pipe for a source, its bottom for a destination. */
	if (stages == 0U && source)
		return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	if (stages == 0U)
		return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	/* A stage 1.0 has no name for. */
	if ((stages & ~RENDER_PASS2_CORE_STAGES) != 0U)
		return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	/* Succeeded: the same stages. */
	return (VkPipelineStageFlags)stages;
}

/* Finds the resolve of depth or stencil a subpass does, NULL when it does none. */
const VkSubpassDescriptionDepthStencilResolve *
vulkan_render_pass2_resolve(
	const VkSubpassDescription2 *subpass)
{
	const VkBaseInStructure *next;
	const VkSubpassDescriptionDepthStencilResolve *resolve;

	/* The record chained to the subpass, naming an attachment and a mode. */
	for (next = subpass->pNext; next != NULL; next = next->pNext) {
		if (next->sType != VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE)
			continue;
		resolve = (const VkSubpassDescriptionDepthStencilResolve *)next;
		if (resolve->pDepthStencilResolveAttachment == NULL ||
		    resolve->pDepthStencilResolveAttachment->attachment == VK_ATTACHMENT_UNUSED)
			return NULL;
		if (resolve->depthResolveMode == VK_RESOLVE_MODE_NONE && resolve->stencilResolveMode == VK_RESOLVE_MODE_NONE)
			return NULL;
		return resolve;
	}

	/* No resolve of depth or stencil. */
	return NULL;
}

/* Encodes one VkAttachmentReference2 without its chain, its layout as Vulkan 1.0 names it. */
static void
render_pass2_encode_reference(
	struct vulkan_writer *writer,
	const VkAttachmentReference2 *reference)
{
	/* The header, the attachment, its layout and its aspects. */
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2);
	vulkan_write_u64(writer, 0U);
	vulkan_write_u32(writer, reference->attachment);
	vulkan_write_u32(writer, vulkan_wire_image_layout(reference->layout));
	vulkan_write_u32(writer, reference->aspectMask);
}

/* Encodes an array of VkAttachmentReference2, its length 0 when it is absent. */
static void
render_pass2_encode_references(
	struct vulkan_writer *writer,
	const VkAttachmentReference2 *references,
	uint32_t count)
{
	uint32_t index;

	/* The length, then each reference. */
	vulkan_write_u64(writer, references != NULL ? count : 0U);
	for (index = 0; references != NULL && index < count; index++)
		render_pass2_encode_reference(writer, &references[index]);
}

/* Finds a queue of the device whose family runs graphics or compute work, NULL when none does. */
static struct VkQueue_T *
core12_query_queue(
	struct VkDevice_T *device)
{
	struct VkPhysicalDevice_T *physical;
	struct VkQueue_T *queue;
	uint32_t index;

	/* The first such queue the device made. */
	physical = device->physical;
	for (index = 0; index < device->queue_count; index++) {
		queue = device->queues[index];
		if (queue->family >= physical->queue_family_count)
			continue;
		if ((physical->queue_families[queue->family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0U)
			return queue;
	}

	/* None. */
	return NULL;
}

/* Gives the timeline state of a semaphore, NULL for a binary one. */
static struct vulkan_timeline *
core12_timeline_of(
	VkSemaphore semaphore)
{
	struct vulkan_sync *sync;

	/* The library's record of the semaphore. */
	sync = vulkan_sync_object((uint64_t)(uintptr_t)semaphore);
	if (sync == NULL)
		return NULL;

	/* Succeeded: its timeline, if it is one. */
	return sync->timeline;
}

/* Drops one reference to a batch's fence, destroying it with the last. */
static void
core12_fence_release(
	struct core12_fence *fence)
{
	/* Others still hold it. */
	fence->references--;
	if (fence->references != 0U)
		return;

	/* The last: the fence goes. */
	vkDestroyFence((VkDevice)fence->device, fence->fence, NULL);
	free(fence);
}

/*
 * Tells whether a batch's fence has signaled: VK_SUCCESS when it has,
 * VK_NOT_READY when not yet, else the failure.  The caller holds the
 * timeline mutex.
 */
static VkResult
core12_fence_check(
	struct core12_fence *fence)
{
	VkResult status;

	/* Asked once more until it signals. */
	if (fence->signaled)
		return VK_SUCCESS;
	status = vkGetFenceStatus((VkDevice)fence->device, fence->fence);
	if (status == VK_SUCCESS)
		fence->signaled = VK_TRUE;

	/* Succeeded, not yet, or refused. */
	return status;
}

/*
 * Gives a timeline the values of its batches that completed: each signal
 * whose fence signaled raises the value and is forgotten.  The caller
 * holds the timeline mutex.
 */
static VkResult
core12_refresh(
	struct vulkan_timeline *timeline)
{
	struct core12_signal **link;
	struct core12_signal *signal;
	VkResult status;

	/* Each pending signal, its fence asked once. */
	link = &timeline->signals;
	while (*link != NULL) {
		signal = *link;
		status = core12_fence_check(signal->fence);
		if (status != VK_SUCCESS && status != VK_NOT_READY)
			return status;

		/* Not yet. */
		if (status == VK_NOT_READY) {
			link = &signal->next;
			continue;
		}

		/* Completed: the value is reached. */
		if (signal->value > timeline->value)
			timeline->value = signal->value;
		*link = signal->next;
		core12_fence_release(signal->fence);
		free(signal);
	}

	/* Succeeded: the value is current. */
	return VK_SUCCESS;
}

/* Gives the library's state of a native driver's event, NULL for none. */
static struct vulkan_event *
core12_event_of(
	uint64_t handle)
{
	struct vulkan_sync *sync;

	/* The library's record of the event. */
	sync = vulkan_sync_object(handle);
	if (sync == NULL)
		return NULL;

	/* Succeeded: its state, if the library keeps it. */
	return sync->library_event;
}

/*
 * Gives an event the changes of its batches that completed, oldest first,
 * each forgotten once made.  The caller holds the timeline mutex.
 */
static VkResult
core12_event_refresh(
	struct vulkan_event *event)
{
	struct core12_event_change **link;
	struct core12_event_change *change;
	VkResult status;

	/* Each pending change, its fence asked once. */
	link = &event->changes;
	while (*link != NULL) {
		change = *link;
		status = core12_fence_check(change->fence);
		if (status != VK_SUCCESS && status != VK_NOT_READY)
			return status;

		/* Not yet. */
		if (status == VK_NOT_READY) {
			link = &change->next;
			continue;
		}

		/* Completed: the event has its state. */
		event->set = change->set;
		*link = change->next;
		core12_fence_release(change->fence);
		free(change);
	}

	/* Succeeded: the state is current. */
	return VK_SUCCESS;
}

/*
 * Tells whether a batch sets or resets an event before one of its actions
 * (the action of that index in the command buffer of that index), and in
 * *set the last such change.
 */
static VkBool32
core12_event_earlier(
	const VkSubmitInfo *submit,
	uint32_t buffer,
	uint32_t action,
	uint64_t handle,
	VkBool32 *set)
{
	struct VkCommandBuffer_T *command;
	const struct vulkan_event_record *record;
	VkBool32 found;
	uint32_t index;
	uint32_t item;
	uint32_t last;

	/* The command buffers before it, then its own actions before it. */
	found = VK_FALSE;
	for (index = 0; index <= buffer; index++) {
		command = (struct VkCommandBuffer_T *)submit->pCommandBuffers[index];
		last = command->event_count;
		if (index == buffer)
			last = action;
		for (item = 0; item < last; item++) {
			record = &command->events[item];
			if (record->event != handle || record->action == VULKAN_EVENT_WAIT)
				continue;
			found = VK_TRUE;
			*set = record->action == VULKAN_EVENT_SET;
		}
	}

	/* Succeeded: whether one was found. */
	return found;
}

/*
 * Tells how a batch's waits for events can be met: now (each event is
 * set, or set by an earlier command of the batch or by a batch sent before
 * it on the same queue), after the fence of another queue's batch that
 * sets one (*fence), or not yet (only the host can set one).
 */
static VkResult
core12_events_ready(
	struct VkQueue_T *queue,
	const VkSubmitInfo *submit,
	enum core12_ready *ready,
	struct core12_fence **fence)
{
	struct VkCommandBuffer_T *command;
	const struct vulkan_event_record *record;
	struct core12_event_change *last;
	struct vulkan_event *event;
	VkBool32 set;
	VkResult status;
	uint32_t index;
	uint32_t item;

	/* Each wait of each command buffer. */
	*ready = CORE12_READY;
	*fence = NULL;
	for (index = 0; index < submit->commandBufferCount; index++) {
		command = (struct VkCommandBuffer_T *)submit->pCommandBuffers[index];
		for (item = 0; item < command->event_count; item++) {
			record = &command->events[item];
			if (record->action != VULKAN_EVENT_WAIT)
				continue;
			event = core12_event_of(record->event);
			if (event == NULL)
				continue;

			/* Set before it in the batch. */
			set = VK_FALSE;
			if (core12_event_earlier(submit, index, item, record->event, &set) && set)
				continue;
			status = core12_event_refresh(event);
			if (status != VK_SUCCESS)
				return status;

			/*
			 * A submitted batch's change comes last: a set of this
			 * queue's is before it, another queue's is waited for.
			 */
			last = event->changes;
			while (last != NULL && last->next != NULL)
				last = last->next;
			if (last != NULL && last->set && last->queue == queue)
				continue;
			if (last != NULL && last->set) {
				*ready = CORE12_AFTER_FENCE;
				*fence = last->fence;
				return VK_SUCCESS;
			}

			/* Set now (and no submitted batch resets it), or only the host can. */
			if (last == NULL && event->set)
				continue;
			*ready = CORE12_NOT_YET;
			return VK_SUCCESS;
		}
	}

	/* Succeeded: the waits can be met. */
	return VK_SUCCESS;
}

/* Tells whether a batch sets or resets a native driver's event. */
static VkBool32
core12_batch_changes(
	const VkSubmitInfo *submit)
{
	struct VkCommandBuffer_T *command;
	uint32_t index;
	uint32_t item;

	/* Each action of each command buffer. */
	for (index = 0; index < submit->commandBufferCount; index++) {
		command = (struct VkCommandBuffer_T *)submit->pCommandBuffers[index];
		for (item = 0; item < command->event_count; item++) {
			if (command->events[item].action != VULKAN_EVENT_WAIT)
				return VK_TRUE;
		}
	}

	/* None. */
	return VK_FALSE;
}

/*
 * Adds the sets and resets of a sent batch, in order, to their events'
 * changes, each made when the batch's fence signals.  The caller holds the
 * timeline mutex and a reference to the fence.
 */
static VkResult
core12_changes_add(
	struct VkQueue_T *queue,
	const VkSubmitInfo *submit,
	struct core12_fence *fence)
{
	struct VkCommandBuffer_T *command;
	const struct vulkan_event_record *record;
	struct core12_event_change *change;
	struct core12_event_change **link;
	struct vulkan_event *event;
	uint32_t index;
	uint32_t item;

	/* Each set and reset of each command buffer. */
	for (index = 0; index < submit->commandBufferCount; index++) {
		command = (struct VkCommandBuffer_T *)submit->pCommandBuffers[index];
		for (item = 0; item < command->event_count; item++) {
			record = &command->events[item];
			if (record->action == VULKAN_EVENT_WAIT)
				continue;
			event = core12_event_of(record->event);
			if (event == NULL)
				continue;
			change = calloc(1, sizeof(*change));
			if (change == NULL)
				return VK_ERROR_OUT_OF_HOST_MEMORY;
			change->set = record->action == VULKAN_EVENT_SET;
			change->queue = queue;
			change->fence = fence;
			fence->references++;

			/* After the changes submitted before it. */
			link = &event->changes;
			while (*link != NULL)
				link = &(*link)->next;
			*link = change;
		}
	}

	/* Succeeded: the changes are known. */
	return VK_SUCCESS;
}

/* Tells whether batches of a queue are held back. */
static VkBool32
core12_queue_held(
	struct VkQueue_T *queue)
{
	struct core12_deferred *entry;

	/* Any held-back batch of the queue. */
	for (entry = core12_deferred_head; entry != NULL; entry = entry->next) {
		if (entry->queue == queue)
			return VK_TRUE;
	}

	/* None. */
	return VK_FALSE;
}

/* Finds the semaphores' values of a batch, NULL when it chains none. */
static const VkTimelineSemaphoreSubmitInfo *
core12_batch_values(
	const VkSubmitInfo *submit)
{
	const VkBaseInStructure *next;

	/* The record chained to the batch. */
	for (next = submit->pNext; next != NULL; next = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
			return (const VkTimelineSemaphoreSubmitInfo *)next;
	}

	/* None. */
	return NULL;
}

/*
 * Tells how a batch of a queue can go to the renderer: now (every value it
 * waits for is reached, or a batch sent before it on the same queue
 * signals it), after the fence of another queue's batch that signals one
 * (*fence), or not yet (nothing submitted signals one).
 */
static VkResult
core12_batch_ready(
	struct VkQueue_T *queue,
	const VkSubmitInfo *submit,
	enum core12_ready *ready,
	struct core12_fence **fence)
{
	const VkTimelineSemaphoreSubmitInfo *values;
	struct vulkan_timeline *timeline;
	struct core12_signal *signal;
	struct core12_signal *found;
	VkResult status;
	uint64_t wanted;
	uint32_t index;

	/* Each timeline the batch waits for. */
	*ready = CORE12_READY;
	*fence = NULL;
	values = core12_batch_values(submit);
	for (index = 0; index < submit->waitSemaphoreCount; index++) {
		timeline = core12_timeline_of(submit->pWaitSemaphores[index]);
		if (timeline == NULL)
			continue;
		wanted = 0;
		if (values != NULL && index < values->waitSemaphoreValueCount)
			wanted = values->pWaitSemaphoreValues[index];
		status = core12_refresh(timeline);
		if (status != VK_SUCCESS)
			return status;
		if (timeline->value >= wanted)
			continue;

		/* A submitted batch that will signal the value: of this queue, or else another's. */
		found = NULL;
		for (signal = timeline->signals; signal != NULL; signal = signal->next) {
			if (signal->value < wanted)
				continue;
			if (found == NULL || signal->queue == queue)
				found = signal;
		}
		if (found == NULL) {
			*ready = CORE12_NOT_YET;
			return VK_SUCCESS;
		}
		if (found->queue != queue) {
			*ready = CORE12_AFTER_FENCE;
			*fence = found->fence;
			return VK_SUCCESS;
		}
	}

	/* The events it waits for. */
	status = core12_events_ready(queue, submit, ready, fence);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the batch can go now, after a fence, or not yet. */
	return VK_SUCCESS;
}

/*
 * Sends one batch to the renderer without its timeline semaphores (fence
 * is the submission's, for its last batch), then, if it signals timelines
 * or sets or resets events, a fence of the library's that tells when it
 * completes.
 */
static VkResult
core12_batch_send(
	struct VkQueue_T *queue,
	const VkSubmitInfo *submit,
	VkFence fence)
{
	const VkTimelineSemaphoreSubmitInfo *values;
	struct vulkan_timeline *timeline;
	struct core12_signal *signal;
	struct core12_fence *completion;
	VkFenceCreateInfo fence_info;
	VkSubmitInfo binary;
	VkSemaphore *semaphores;
	VkPipelineStageFlags *stages;
	VkBool32 signals;
	VkBool32 changes;
	VkResult status;
	uint32_t index;
	uint32_t waits;
	uint32_t count;

	/* Room for the binary semaphores and the wait stages. */
	semaphores = calloc((size_t)submit->waitSemaphoreCount + submit->signalSemaphoreCount + 1U, sizeof(*semaphores));
	stages = calloc((size_t)submit->waitSemaphoreCount + 1U, sizeof(*stages));
	if (semaphores == NULL || stages == NULL) {
		free(stages);
		free(semaphores);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The batch with its binary semaphores only. */
	binary = *submit;
	binary.pNext = NULL;
	waits = 0;
	for (index = 0; index < submit->waitSemaphoreCount; index++) {
		if (core12_timeline_of(submit->pWaitSemaphores[index]) != NULL)
			continue;
		semaphores[waits] = submit->pWaitSemaphores[index];
		stages[waits] = submit->pWaitDstStageMask[index];
		waits++;
	}
	binary.waitSemaphoreCount = waits;
	binary.pWaitSemaphores = semaphores;
	binary.pWaitDstStageMask = stages;
	count = 0;
	signals = VK_FALSE;
	for (index = 0; index < submit->signalSemaphoreCount; index++) {
		if (core12_timeline_of(submit->pSignalSemaphores[index]) != NULL) {
			signals = VK_TRUE;
			continue;
		}
		semaphores[waits + count] = submit->pSignalSemaphores[index];
		count++;
	}
	binary.signalSemaphoreCount = count;
	binary.pSignalSemaphores = semaphores + waits;

	/* Sent. */
	status = vulkan_queue_submit(queue, 1U, &binary, fence);
	free(stages);
	free(semaphores);
	changes = core12_batch_changes(submit);
	if (status != VK_SUCCESS || (!signals && !changes))
		return status;

	/* The library's fence after it, which signals when it completes. */
	completion = calloc(1, sizeof(*completion));
	if (completion == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	completion->device = queue->device;
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence((VkDevice)queue->device, &fence_info, NULL, &completion->fence);
	if (status == VK_SUCCESS)
		status = vulkan_queue_submit(queue, 0U, NULL, completion->fence);
	if (status != VK_SUCCESS) {
		vkDestroyFence((VkDevice)queue->device, completion->fence, NULL);
		free(completion);
		return status;
	}

	/* Each timeline it signals, with its value. */
	values = core12_batch_values(submit);
	completion->references = 1U;
	for (index = 0; index < submit->signalSemaphoreCount; index++) {
		timeline = core12_timeline_of(submit->pSignalSemaphores[index]);
		if (timeline == NULL)
			continue;
		signal = calloc(1, sizeof(*signal));
		if (signal == NULL) {
			status = VK_ERROR_OUT_OF_HOST_MEMORY;
			break;
		}
		signal->value = 0;
		if (values != NULL && index < values->signalSemaphoreValueCount)
			signal->value = values->pSignalSemaphoreValues[index];
		signal->queue = queue;
		signal->fence = completion;
		completion->references++;
		signal->next = timeline->signals;
		timeline->signals = signal;
	}

	/* Each event it sets or resets, in order. */
	if (status == VK_SUCCESS)
		status = core12_changes_add(queue, submit, completion);
	core12_fence_release(completion);
	pthread_cond_broadcast(&core12_timeline_cond);

	/* Refused, or succeeded: the batch is sent and its signals are known. */
	return status;
}

/*
 * Sends a submission's batches in turn, held back from the first that
 * cannot go yet (or from the first, when held is true), waiting for
 * another queue's batch when one must complete first.  The caller holds the
 * timeline mutex, which this releases while it waits.
 */
static VkResult
core12_submit_from(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	VkFence fence,
	VkBool32 held)
{
	struct core12_fence *before;
	enum core12_ready ready;
	VkResult status;
	VkFence last;
	uint32_t index;

	/* Each batch in order. */
	index = 0;
	while (index < count) {
		last = VK_NULL_HANDLE;
		if (index + 1U == count)
			last = fence;

		/* Held back: this and every later batch. */
		ready = CORE12_NOT_YET;
		before = NULL;
		if (!held) {
			status = core12_batch_ready(queue, &submits[index], &ready, &before);
			if (status != VK_SUCCESS)
				return status;
		}
		if (ready == CORE12_NOT_YET) {
			status = core12_defer(queue, &submits[index], last);
			if (status != VK_SUCCESS)
				return status;
			held = VK_TRUE;
			index++;
			continue;
		}

		/* Another queue's batch first. */
		if (ready == CORE12_AFTER_FENCE) {
			before->references++;
			pthread_mutex_unlock(&core12_timeline_mutex);
			status = vkWaitForFences((VkDevice)queue->device, 1U, &before->fence, VK_TRUE, UINT64_MAX);
			pthread_mutex_lock(&core12_timeline_mutex);
			core12_fence_release(before);
			if (status != VK_SUCCESS)
				return status;
			continue;
		}

		/* Sent now. */
		status = core12_batch_send(queue, &submits[index], last);
		if (status != VK_SUCCESS)
			return status;
		index++;
	}

	/* Succeeded: every batch is sent or held back. */
	return VK_SUCCESS;
}

/* Holds a batch back: a copy of it and its arrays at the end of the held-back batches. */
static VkResult
core12_defer(
	struct VkQueue_T *queue,
	const VkSubmitInfo *submit,
	VkFence fence)
{
	const VkTimelineSemaphoreSubmitInfo *values;
	struct core12_deferred *entry;
	struct core12_deferred **link;
	VkSemaphore *semaphores;
	VkCommandBuffer *commands;
	uint64_t *numbers;
	VkPipelineStageFlags *stages;
	uint32_t waits;
	uint32_t signals;
	uint32_t index;
	size_t bytes;
	char *block;

	/* One block: the values, the semaphores, the command buffers, then the stages. */
	waits = submit->waitSemaphoreCount;
	signals = submit->signalSemaphoreCount;
	bytes = ((size_t)waits + signals) * (sizeof(uint64_t) + sizeof(VkSemaphore));
	bytes += (size_t)submit->commandBufferCount * sizeof(VkCommandBuffer);
	bytes += (size_t)waits * sizeof(VkPipelineStageFlags);
	entry = calloc(1, sizeof(*entry));
	block = calloc(1, bytes + 1U);
	if (entry == NULL || block == NULL) {
		free(block);
		free(entry);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	numbers = (uint64_t *)block;
	semaphores = (VkSemaphore *)(numbers + waits + signals);
	commands = (VkCommandBuffer *)(semaphores + waits + signals);
	stages = (VkPipelineStageFlags *)(commands + submit->commandBufferCount);

	/* The copy. */
	values = core12_batch_values(submit);
	for (index = 0; index < waits; index++) {
		semaphores[index] = submit->pWaitSemaphores[index];
		stages[index] = submit->pWaitDstStageMask[index];
		if (values != NULL && index < values->waitSemaphoreValueCount)
			numbers[index] = values->pWaitSemaphoreValues[index];
	}
	for (index = 0; index < signals; index++) {
		semaphores[waits + index] = submit->pSignalSemaphores[index];
		if (values != NULL && index < values->signalSemaphoreValueCount)
			numbers[waits + index] = values->pSignalSemaphoreValues[index];
	}
	for (index = 0; index < submit->commandBufferCount; index++)
		commands[index] = submit->pCommandBuffers[index];
	entry->queue = queue;
	entry->fence = fence;
	entry->storage = block;
	entry->timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	entry->timeline.waitSemaphoreValueCount = waits;
	entry->timeline.pWaitSemaphoreValues = numbers;
	entry->timeline.signalSemaphoreValueCount = signals;
	entry->timeline.pSignalSemaphoreValues = numbers + waits;
	entry->submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	entry->submit.pNext = &entry->timeline;
	entry->submit.waitSemaphoreCount = waits;
	entry->submit.pWaitSemaphores = semaphores;
	entry->submit.pWaitDstStageMask = stages;
	entry->submit.commandBufferCount = submit->commandBufferCount;
	entry->submit.pCommandBuffers = commands;
	entry->submit.signalSemaphoreCount = signals;
	entry->submit.pSignalSemaphores = semaphores + waits;

	/* At the end, after every batch held back before it. */
	link = &core12_deferred_head;
	while (*link != NULL)
		link = &(*link)->next;
	*link = entry;

	/* Succeeded: the batch waits in the library. */
	return VK_SUCCESS;
}

/*
 * Sends the held-back batches that can go now, oldest first, each only
 * when no older batch of its queue is still held back, until none can.  A
 * thread that finds another sending them leaves it to look once more.  The
 * caller holds the timeline mutex.
 */
static void
core12_flush(void)
{
	struct core12_deferred **link;
	struct core12_deferred *entry;
	struct core12_deferred *older;
	struct core12_fence *before;
	enum core12_ready ready;
	VkBool32 blocked;
	VkResult status;

	/* Another thread is sending them: it looks once more. */
	if (core12_flushing) {
		core12_flush_again = VK_TRUE;
		return;
	}
	core12_flushing = VK_TRUE;

	/* Until a whole look sends nothing. */
	do {
		core12_flush_again = VK_FALSE;
		for (link = &core12_deferred_head; *link != NULL; link = &(*link)->next) {
			entry = *link;

			/* An older batch of the same queue goes first. */
			blocked = VK_FALSE;
			for (older = core12_deferred_head; older != entry; older = older->next) {
				if (older->queue == entry->queue)
					blocked = VK_TRUE;
			}
			if (blocked)
				continue;

			/* Not yet. */
			status = core12_batch_ready(entry->queue, &entry->submit, &ready, &before);
			if (status != VK_SUCCESS || ready == CORE12_NOT_YET)
				continue;

			/* Another queue's batch first; the list may change meanwhile, so look again. */
			if (ready == CORE12_AFTER_FENCE) {
				before->references++;
				pthread_mutex_unlock(&core12_timeline_mutex);
				(void)vkWaitForFences((VkDevice)entry->queue->device, 1U, &before->fence, VK_TRUE, UINT64_MAX);
				pthread_mutex_lock(&core12_timeline_mutex);
				core12_fence_release(before);
				core12_flush_again = VK_TRUE;
				break;
			}

			/* Sent and forgotten; a failure is the queue's, as a lost device. */
			*link = entry->next;
			status = core12_batch_send(entry->queue, &entry->submit, entry->fence);
			if (status != VK_SUCCESS)
				vulkan_sync_device_error(entry->queue->device, status);
			free(entry->storage);
			free(entry);
			core12_flush_again = VK_TRUE;
			break;
		}
	} while (core12_flush_again);
	core12_flushing = VK_FALSE;
}

/*
 * Tells whether a host wait's values are reached (any one, or all), and
 * gives in *fence a batch's fence that would give one of those not
 * reached (NULL when only a host signal can).  The caller holds the
 * timeline mutex.
 */
static VkResult
core12_wait_reached(
	const VkSemaphoreWaitInfo *info,
	VkBool32 *reached,
	struct core12_fence **fence)
{
	struct vulkan_timeline *timeline;
	struct core12_signal *signal;
	VkBool32 any;
	VkBool32 all;
	VkResult status;
	uint32_t index;

	/* Each semaphore against its value. */
	any = VK_FALSE;
	all = VK_TRUE;
	*fence = NULL;
	for (index = 0; index < info->semaphoreCount; index++) {
		timeline = core12_timeline_of(info->pSemaphores[index]);
		if (timeline == NULL)
			return VK_ERROR_INITIALIZATION_FAILED;
		status = core12_refresh(timeline);
		if (status != VK_SUCCESS)
			return status;
		if (timeline->value >= info->pValues[index]) {
			any = VK_TRUE;
			continue;
		}
		all = VK_FALSE;
		for (signal = timeline->signals; signal != NULL && *fence == NULL; signal = signal->next) {
			if (signal->value >= info->pValues[index])
				*fence = signal->fence;
		}
	}

	/* Any one, or all. */
	*reached = all;
	if ((info->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0U)
		*reached = any;

	/* Succeeded: the answer is given. */
	return VK_SUCCESS;
}

/* Sleeps until a timeline changes or nanoseconds pass.  The caller holds the timeline mutex. */
static void
core12_sleep(
	uint64_t nanoseconds)
{
	struct timespec now;
	struct timespec until;
	uint64_t total;

	/* The condition's clock, plus the time. */
	clock_gettime(CLOCK_REALTIME, &now);
	total = (uint64_t)now.tv_nsec + nanoseconds;
	until.tv_sec = now.tv_sec + (time_t)(total / UINT64_C(1000000000));
	until.tv_nsec = (long)(total % UINT64_C(1000000000));

	/* Woken by a change, or at the time. */
	(void)pthread_cond_timedwait(&core12_timeline_cond, &core12_timeline_mutex, &until);
}
