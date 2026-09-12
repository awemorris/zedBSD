/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements native synchronization with completed WSI payloads and unlocked waits.
 */

#include <errno.h>
#include <string.h>
#include <time.h>
#include "sync-internal.h"

#define VULKAN_SYNC_POLL_NS UINT64_C(1000000)

static VkResult sync_create(struct VkDevice_T *device, enum vulkan_object_kind kind, uint32_t opcode, uint32_t destroy_opcode, uint32_t structure, VkFlags flags, const VkAllocationCallbacks *allocator, struct vulkan_sync **result);
static void sync_destroy(struct VkDevice_T *device, struct vulkan_sync *sync, uint32_t opcode, const VkAllocationCallbacks *allocator);
static VkResult sync_event_operation(VkDevice device, VkEvent event, uint32_t opcode);

/*
 * Creates a fence with the application's requested initial native state.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(
	VkDevice device,
	const VkFenceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFence *pFence)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	uint64_t handle;
	VkResult status;

	/* Allocate one fence using this call's effective allocation policy. */
	owner = vulkan_device(device);
	status = sync_create(owner, VULKAN_OBJECT_FENCE, VULKAN_OPCODE_vkCreateFence, VULKAN_OPCODE_vkDestroyFence, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, pCreateInfo->flags, pAllocator, &sync);
	if (status != VK_SUCCESS)
		return status;

	/* Publish the standard ABI handle only after native creation succeeds. */
	handle = vulkan_nondispatchable_handle(&sync->object);
	*pFence = (VkFence)(uintptr_t)handle;

	/* Succeeded: the fence retains its requested signaled or unsignaled state. */
	return VK_SUCCESS;
}

/*
 * Destroys an externally synchronized fence and its saved allocation.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(
	VkDevice device,
	VkFence fence,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;

	/* The destruction call may supply compatible callbacks with different userdata. */
	owner = vulkan_device(device);
	sync = vulkan_sync_object((uint64_t)(uintptr_t)fence);
	sync_destroy(owner, sync, VULKAN_OPCODE_vkDestroyFence, pAllocator);

	/* Succeeded: null fences are ignored and ordinary fence ownership is consumed. */
	return;
}

/*
 * Resets native fences and completed WSI payloads in one device transaction.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(
	VkDevice device,
	uint32_t fenceCount,
	const VkFence *pFences)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	uint32_t index;

	/* Encode every native fence even if its active payload came from acquisition. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkResetFences);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u32(&writer, fenceCount);
	vulkan_write_u64(&writer, fenceCount);
	for (index = 0; index < fenceCount; index++) {
		sync = vulkan_sync_object((uint64_t)(uintptr_t)pFences[index]);
		vulkan_write_u64(&writer, sync->object.wire_id);
	}

	/* Commit software reset only after the host accepted the native reset. */
	pthread_mutex_lock(&owner->mutex);

	status = vulkan_command_execute(owner->object.context, &writer, 8, &reader, VK_TRUE);
	if (status == VK_SUCCESS) {
		/* A reset fence no longer carries acquisition completion to host waiters. */
		for (index = 0; index < fenceCount; index++) {
			sync = vulkan_sync_object((uint64_t)(uintptr_t)pFences[index]);
			sync->software_signaled = VK_FALSE;
		}
	}

	/* Preserve device loss for future local completion observations. */
	vulkan_sync_device_error(owner, status);

	pthread_mutex_unlock(&owner->mutex);

	/* Release independent reply and encoding storage after the transaction. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: all requested fences now have unsignaled payloads. */
	return VK_SUCCESS;
}

/*
 * Observes a native or completed acquisition fence without a GPU wait.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetFenceStatus(
	VkDevice device,
	VkFence fence)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	VkResult status;

	/* Resolve the ordinary fence object before its brief payload transaction. */
	owner = vulkan_device(device);
	sync = vulkan_sync_object((uint64_t)(uintptr_t)fence);
	pthread_mutex_lock(&owner->mutex);

	status = vulkan_sync_device_status_locked(owner);
	if (status == VK_SUCCESS) {
		/* Software completion is valid only after WSI released the actual image. */
		if (sync->software_signaled) {
			status = VK_SUCCESS;
		} else {
			status = vulkan_sync_status_locked(owner, sync, VULKAN_OPCODE_vkGetFenceStatus);
		}
	}

	pthread_mutex_unlock(&owner->mutex);

	/* Preserve native NOT_READY and device-loss results without coercion. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: this fence's actual active payload is complete. */
	return VK_SUCCESS;
}

/*
 * Waits for the requested fence condition without retaining submission locks.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(
	VkDevice device,
	uint32_t fenceCount,
	const VkFence *pFences,
	VkBool32 waitAll,
	uint64_t timeout)
{
	struct VkDevice_T *owner;
	VkResult status;

	/* Use the same fence semantics for public callers and internal idle markers. */
	owner = vulkan_device(device);
	status = vulkan_fences_wait(owner, fenceCount, pFences, waitAll, timeout);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the caller's all-or-any fence condition is satisfied. */
	return VK_SUCCESS;
}

/*
 * Creates one unsignaled native binary semaphore with no acquisition payload.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSemaphore(
	VkDevice device,
	const VkSemaphoreCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSemaphore *pSemaphore)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	uint64_t handle;
	VkResult status;

	/* Create the native payload before allowing queue or WSI use. */
	owner = vulkan_device(device);
	status = sync_create(owner, VULKAN_OBJECT_SEMAPHORE, VULKAN_OPCODE_vkCreateSemaphore, VULKAN_OPCODE_vkDestroySemaphore, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, pCreateInfo->flags, pAllocator, &sync);
	if (status != VK_SUCCESS)
		return status;

	/* Expose the standard non-dispatchable handle rather than its wire identity. */
	handle = vulkan_nondispatchable_handle(&sync->object);
	*pSemaphore = (VkSemaphore)(uintptr_t)handle;

	/* Succeeded: the ordinary binary semaphore is initially unsignaled. */
	return VK_SUCCESS;
}

/*
 * Destroys a binary semaphore after all submitted uses have completed.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroySemaphore(
	VkDevice device,
	VkSemaphore semaphore,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;

	/* Honor compatible destruction callbacks while consuming this semaphore. */
	owner = vulkan_device(device);
	sync = vulkan_sync_object((uint64_t)(uintptr_t)semaphore);
	sync_destroy(owner, sync, VULKAN_OPCODE_vkDestroySemaphore, pAllocator);

	/* Succeeded: the semaphore's local and native ownership is consumed. */
	return;
}

/*
 * Creates an initially reset event shared by ordinary host and GPU operations.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateEvent(
	VkDevice device,
	const VkEventCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkEvent *pEvent)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	uint64_t handle;
	VkResult status;

	/* Keep event state native so command buffers observe later host changes. */
	owner = vulkan_device(device);
	status = sync_create(owner, VULKAN_OBJECT_EVENT, VULKAN_OPCODE_vkCreateEvent, VULKAN_OPCODE_vkDestroyEvent, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, pCreateInfo->flags, pAllocator, &sync);
	if (status != VK_SUCCESS)
		return status;

	/* Publish only a successfully created event to the application. */
	handle = vulkan_nondispatchable_handle(&sync->object);
	*pEvent = (VkEvent)(uintptr_t)handle;

	/* Succeeded: the native event begins in its reset state. */
	return VK_SUCCESS;
}

/*
 * Destroys an event after the application has synchronized its outstanding uses.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyEvent(
	VkDevice device,
	VkEvent event,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;

	/* Honor the effective destruction allocator, including null-handle cleanup. */
	owner = vulkan_device(device);
	sync = vulkan_sync_object((uint64_t)(uintptr_t)event);
	sync_destroy(owner, sync, VULKAN_OPCODE_vkDestroyEvent, pAllocator);

	/* Succeeded: the application no longer owns this event. */
	return;
}

/*
 * Reads the actual host-visible event state without consuming it.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetEventStatus(
	VkDevice device,
	VkEvent event)
{
	VkResult status;

	/* Preserve EVENT_SET and EVENT_RESET as the event's meaningful answers. */
	status = sync_event_operation(device, event, VULKAN_OPCODE_vkGetEventStatus);
	if (status < 0)
		return status;

	/* Succeeded: report the native event state rather than a cached guess. */
	return status;
}

/*
 * Sets a native event so already submitted GPU waits can make progress.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkSetEvent(
	VkDevice device,
	VkEvent event)
{
	VkResult status;

	/* Execute this short host operation independently of GPU fence waiters. */
	status = sync_event_operation(device, event, VULKAN_OPCODE_vkSetEvent);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the native event is signaled for subsequent dependency checks. */
	return VK_SUCCESS;
}

/*
 * Resets a native event after the application has synchronized prior use.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetEvent(
	VkDevice device,
	VkEvent event)
{
	VkResult status;

	/* Keep host reset visible to the same event used by command buffers. */
	status = sync_event_operation(device, event, VULKAN_OPCODE_vkResetEvent);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the native event is reset. */
	return VK_SUCCESS;
}

/*
 * Signals acquisition completion without placing work behind a blocked queue.
 */
VkResult
vulkan_wsi_acquire_signal(
	struct VkDevice_T *device,
	VkSemaphore semaphore,
	VkFence fence)
{
	struct vulkan_sync *acquire_semaphore;
	struct vulkan_sync *acquire_fence;
	VkResult status;

	/* Resolve optional ordinary synchronization objects before taking ownership. */
	acquire_semaphore = vulkan_sync_object((uint64_t)(uintptr_t)semaphore);
	acquire_fence = vulkan_sync_object((uint64_t)(uintptr_t)fence);
	pthread_mutex_lock(&device->mutex);

	status = vulkan_sync_device_status_locked(device);
	if (status == VK_SUCCESS) {
		/* Queue wait encoding consumes this already completed payload exactly once. */
		if (acquire_semaphore != NULL)
			acquire_semaphore->software_signaled = VK_TRUE;

		/* Fence observation retains completion until an explicit fence reset. */
		if (acquire_fence != NULL)
			acquire_fence->software_signaled = VK_TRUE;
	}

	pthread_mutex_unlock(&device->mutex);

	/* A lost device cannot acquire a newly successful local completion. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: native display release is represented by ordinary sync payloads. */
	return VK_SUCCESS;
}

/*
 * Polls native and acquisition fences with the caller's exact timeout policy.
 */
VkResult
vulkan_fences_wait(
	struct VkDevice_T *device,
	uint32_t count,
	const VkFence *fences,
	VkBool32 all,
	uint64_t timeout_ns)
{
	uint64_t started;
	uint64_t now;
	uint64_t delay;
	uint64_t elapsed;
	uint32_t index;
	uint32_t completed;
	VkResult status;

	/* Measure one deadline before the first observation, including transaction time. */
	status = vulkan_sync_clock(&started);
	if (status != VK_SUCCESS)
		return status;

	/* Give other host threads an opportunity to submit and signal between polls. */
	for (;;) {
		/* Each individual observation releases its device and transport locks. */
		completed = 0;
		for (index = 0; index < count; index++) {
			status = vkGetFenceStatus((VkDevice)device, fences[index]);
			if (status < 0)
				return status;

			/* Count only completed native or acquisition payloads. */
			if (status == VK_SUCCESS) {
				completed++;

				/* Any-fence waiting ends at the first successful observation. */
				if (!all)
					break;
			}
		}

		/* All-fence and any-fence conditions have separate debugger decisions. */
		if (all) {
			/* Every requested fence has been observed complete. */
			if (completed == count)
				break;
		} else {
			/* At least one observed completion satisfies an any-fence wait. */
			if (completed != 0)
				break;
		}

		/* A zero timeout performs exactly one observation pass. */
		if (timeout_ns == 0)
			return VK_TIMEOUT;

		/* Bound ordinary waits without imposing a finite limit on UINT64_MAX. */
		delay = VULKAN_SYNC_POLL_NS;
		if (timeout_ns != UINT64_MAX) {
			/* Count wire processing against the original finite deadline. */
			status = vulkan_sync_clock(&now);
			if (status != VK_SUCCESS)
				return status;

			/* Unsigned subtraction also expires a clock that moves backward. */
			elapsed = now - started;
			if (elapsed >= timeout_ns)
				return VK_TIMEOUT;

			/* Never deliberately sleep beyond the remaining caller deadline. */
			if (delay > timeout_ns - elapsed)
				delay = timeout_ns - elapsed;
		}

		/* Yield without retaining any queue, device, or context mutex. */
		status = vulkan_sync_pause(delay);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Succeeded: the requested fence condition was observed complete. */
	return VK_SUCCESS;
}

/*
 * Converts an ordinary non-dispatchable sync handle into its retained object.
 */
struct vulkan_sync *
vulkan_sync_object(
	uint64_t handle)
{
	struct vulkan_object *object;

	/* Keep all standard handle representation changes in the common helper. */
	object = vulkan_nondispatchable_object(handle);

	/* Succeeded: the common object is the first member of every sync allocation. */
	return (struct vulkan_sync *)object;
}

/*
 * Observes context loss before accepting a locally completed synchronization payload.
 */
VkResult
vulkan_sync_device_status_locked(
	struct VkDevice_T *device)
{
	VkResult status;

	/* Device-local loss and transport loss both invalidate subsequent completion. */
	if (device->error != VK_SUCCESS)
		return device->error;

	/* Other API families publish context failure without taking this device mutex. */
	status = __atomic_load_n(&device->object.context->error, __ATOMIC_ACQUIRE);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: no earlier device or shared transport failure is hidden. */
	return VK_SUCCESS;
}

/*
 * Executes one native status or event operation while its caller owns the device.
 */
VkResult
vulkan_sync_status_locked(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	uint32_t opcode)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Encode only renderer identities, never application pointers or handles. */
	vulkan_writer_init_for_object(&writer, &sync->object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, sync->object.wire_id);
	status = vulkan_command_execute(device->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_sync_device_error(device, status);

	/* Both independent buffers expire after this short transaction. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	if (status < 0)
		return status;

	/* Succeeded: preserve the native status, including an unsignaled payload. */
	return status;
}

/*
 * Records device loss before a later local payload can report success.
 */
void
vulkan_sync_device_error(
	struct VkDevice_T *device,
	VkResult status)
{
	/* Host allocation pressure does not by itself invalidate the Vulkan device. */
	if (status == VK_ERROR_DEVICE_LOST) {
		device->error = status;
		__atomic_store_n(&device->object.context->error, status, __ATOMIC_RELEASE);
	}

	/* Succeeded: device-loss state remains sticky for all sync observers. */
	return;
}

/*
 * Samples a monotonic nanosecond clock without retaining Vulkan locks.
 */
VkResult
vulkan_sync_clock(
	uint64_t *nanoseconds)
{
	struct timespec sample;
	int error;

	/* A failed timing source cannot safely implement a Vulkan timeout. */
	error = clock_gettime(CLOCK_MONOTONIC, &sample);
	if (error != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Guard the nanosecond conversion before multiplying an arbitrary epoch. */
	if ((uint64_t)sample.tv_sec > UINT64_MAX / UINT64_C(1000000000))
		return VK_ERROR_DEVICE_LOST;

	/* Bound the final fractional second independently of the multiplication. */
	*nanoseconds = (uint64_t)sample.tv_sec * UINT64_C(1000000000);
	if ((uint64_t)sample.tv_nsec > UINT64_MAX - *nanoseconds)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: expose one monotonic sample with nanosecond timeout units. */
	*nanoseconds += (uint64_t)sample.tv_nsec;
	return VK_SUCCESS;
}

/*
 * Yields between nonblocking observations while leaving all Vulkan locks free.
 */
VkResult
vulkan_sync_pause(
	uint64_t nanoseconds)
{
	struct timespec delay;
	int error;

	/* Polling delays never exceed one quantum, even for indefinite Vulkan waits. */
	if (nanoseconds > VULKAN_SYNC_POLL_NS)
		nanoseconds = VULKAN_SYNC_POLL_NS;

	/* Let an interrupt shorten this pause because the caller rechecks its deadline. */
	delay.tv_sec = 0;
	delay.tv_nsec = (long)nanoseconds;
	error = nanosleep(&delay, NULL);
	if (error != 0) {
		/* An ordinary signal is an opportunity to observe completion again. */
		if (errno != EINTR)
			return VK_ERROR_DEVICE_LOST;
	}

	/* Succeeded: the caller can perform another short completion observation. */
	return VK_SUCCESS;
}

/* Create one flags-only native sync object before publishing its local identity. */
static VkResult
sync_create(
	struct VkDevice_T *device,
	enum vulkan_object_kind kind,
	uint32_t opcode,
	uint32_t destroy_opcode,
	uint32_t structure,
	VkFlags flags,
	const VkAllocationCallbacks *allocator,
	struct vulkan_sync **result)
{
	struct vulkan_object *object;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint64_t present;
	uint64_t returned;
	VkResult status;

	/* Retain one callback-owned local object through every creation rollback. */
	status = vulkan_object_alloc(sizeof(struct vulkan_sync), sizeof(uint64_t), kind, &device->object, device->object.context, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* Reserve a never-reused renderer identity before encoding its output pointer. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* The three core flags-only create structures share this explicit wire shape. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, structure);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u32(&writer, flags);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, object->wire_id);
	status = vulkan_command_execute(device->object.context, &writer, 24, &reader, VK_TRUE);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(device->object.context, &reader, status);
		vulkan_writer_finish(&writer);
		vulkan_object_free(object);
		return status;
	}

	/* A successful host creation must echo the reserved output identity. */
	present = vulkan_read_u64(&reader);
	returned = vulkan_read_u64(&reader);
	status = reader.error;
	if (status == VK_SUCCESS) {
		/* A missing required pointer is a protocol failure, not a null object. */
		if (present != 1)
			status = VK_ERROR_DEVICE_LOST;

		/* A foreign identity must never be exposed as this local object. */
		if (returned != object->wire_id)
			status = VK_ERROR_DEVICE_LOST;
	}

	/* Protocol loss is shared so no software completion can mask malformed creation. */
	if (status == VK_ERROR_DEVICE_LOST)
		__atomic_store_n(&device->object.context->error, status, __ATOMIC_RELEASE);

	/* Native ownership already exists; use its normal destruction for rollback. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		sync_destroy(device, (struct vulkan_sync *)object, destroy_opcode, NULL);
		return status;
	}

	/* Link the complete object only after both host state and reply are valid. */
	status = vulkan_object_publish(object);
	if (status != VK_SUCCESS) {
		sync_destroy(device, (struct vulkan_sync *)object, destroy_opcode, NULL);
		return status;
	}

	/* Succeeded: the caller receives one fully owned ordinary sync object. */
	*result = (struct vulkan_sync *)object;
	return VK_SUCCESS;
}

/* Release a flags-only native synchronization object and its saved allocator. */
static void
sync_destroy(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	uint32_t opcode,
	const VkAllocationCallbacks *allocator)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* Vulkan destruction accepts the null handle during partial initialization. */
	if (sync == NULL)
		return;

	/* Keep native destruction and software payload retirement in one transaction. */
	vulkan_writer_init_for_object(&writer, &sync->object);
	if (allocator != NULL) {
		writer.allocator.callbacks = *allocator;
		writer.allocator.has_callbacks = VK_TRUE;
	}

	/* Encode the native destruction after selecting this call's command allocator. */
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, sync->object.wire_id);
	vulkan_write_u64(&writer, 0);
	pthread_mutex_lock(&device->mutex);

	status = vulkan_command_execute(device->object.context, &writer, 4, &reader, VK_FALSE);
	vulkan_sync_device_error(device, status);

	pthread_mutex_unlock(&device->mutex);

	/* Void destruction consumes local ownership even after device loss. */
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	vulkan_object_free_with_allocator(&sync->object, allocator);

	/* Succeeded: no local sync allocation remains owned by the application. */
	return;
}

/* Apply a native host event operation without holding locks while a GPU waits. */
static VkResult
sync_event_operation(
	VkDevice device,
	VkEvent event,
	uint32_t opcode)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	VkResult status;

	/* Resolve the event's ordinary ownership record before the short transaction. */
	owner = vulkan_device(device);
	sync = vulkan_sync_object((uint64_t)(uintptr_t)event);
	pthread_mutex_lock(&owner->mutex);

	status = vulkan_sync_device_status_locked(owner);
	if (status == VK_SUCCESS)
		status = vulkan_sync_status_locked(owner, sync, opcode);

	pthread_mutex_unlock(&owner->mutex);

	/* Preserve native event answers, including SET and RESET statuses. */
	if (status < 0)
		return status;

	/* Succeeded: the event operation completed or reported its actual state. */
	return status;
}
