/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares OPAQUE_FD payloads whose admitted GPU work is completed by the kernel.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/gpu-fence.h>
#include "sync-internal.h"

/* One externally synchronized fence retains both permanent and temporary references. */
struct vulkan_external_fence {
	int permanent;
	int temporary;
};

static VkResult external_enable(struct VkDevice_T *device, struct vulkan_sync *sync, VkBool32 signaled);
static int external_active(struct vulkan_external_fence *external);
static VkResult external_query(struct VkDevice_T *device, int fd, struct gpu_fence_state *state);
static VkResult external_errno(int error);

/*
 * Reports only the reference-bearing handle implemented by this kernel device.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceExternalFencePropertiesKHR(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
	VkExternalFenceProperties *pExternalFenceProperties)
{
	struct VkPhysicalDevice_T *physical;

	/* Preserve caller-owned structure framing while clearing unsupported features. */
	physical = vulkan_physical_device(physicalDevice);
	pExternalFenceProperties->exportFromImportedHandleTypes = 0;
	pExternalFenceProperties->compatibleHandleTypes = 0;
	pExternalFenceProperties->externalFenceFeatures = 0;
	if (pExternalFenceInfo->handleType != VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT)
		return;

	/* A native renderer fence alone does not provide guest descriptor sharing. */
	if ((physical->object.context->capabilities & GPU_CAP_FENCE) == 0)
		return;

	/* Succeeded: the same OPAQUE payload can be imported, exported and exported again. */
	pExternalFenceProperties->exportFromImportedHandleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	pExternalFenceProperties->compatibleHandleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
	pExternalFenceProperties->externalFenceFeatures = VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
	return;
}

/*
 * Returns a fresh close-on-exec descriptor without consuming the fence payload.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetFenceFdKHR(
	VkDevice device,
	const VkFenceGetFdInfoKHR *pGetFdInfo,
	int *pFd)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	VkResult status;
	int active;
	int descriptor;

	/* Reject unsupported handle families before exposing a descriptor. */
	owner = vulkan_device(device);
	if (pGetFdInfo->handleType != VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	sync = vulkan_sync_object((uint64_t)(uintptr_t)pGetFdInfo->fence);
	if (sync->external == NULL || (sync->export_types & VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT) == 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Serialize payload selection with imports and resets of this local fence. */
	pthread_mutex_lock(&owner->mutex);

	status = vulkan_sync_device_status_locked(owner);
	descriptor = -1;
	if (status == VK_SUCCESS) {
		active = external_active(sync->external);
		descriptor = fcntl(active, F_DUPFD_CLOEXEC, 0);
		if (descriptor < 0)
			status = external_errno(errno);
	}

	pthread_mutex_unlock(&owner->mutex);

	/* Descriptor pressure leaves the selected payload and caller ownership unchanged. */
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the application owns an independent reference to the active payload. */
	*pFd = descriptor;
	return VK_SUCCESS;
}

/*
 * Takes descriptor ownership only after the complete reference import succeeds.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkImportFenceFdKHR(
	VkDevice device,
	const VkImportFenceFdInfoKHR *pImportFenceFdInfo)
{
	struct VkDevice_T *owner;
	struct vulkan_sync *sync;
	struct vulkan_external_fence *external;
	struct gpu_fence_state state;
	VkResult status;
	VkBool32 signaled;
	int retired;

	/* OPAQUE descriptors represent same-device reference payloads, never sync_file copies. */
	owner = vulkan_device(device);
	if (pImportFenceFdInfo->handleType != VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT || pImportFenceFdInfo->fd < 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	if ((pImportFenceFdInfo->flags & ~VK_FENCE_IMPORT_TEMPORARY_BIT) != 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Validate kernel type and physical-device identity before changing local ownership. */
	status = external_query(owner, pImportFenceFdInfo->fd, &state);
	if (status != VK_SUCCESS)
		return status;

	/* A previous native marker must stop borrowing this fence before import replaces its payload. */
	sync = vulkan_sync_object((uint64_t)(uintptr_t)pImportFenceFdInfo->fence);
	vulkan_sync_quiesce(sync);
	pthread_mutex_lock(&owner->mutex);

	status = vulkan_sync_device_status_locked(owner);
	if (status == VK_SUCCESS && sync->external == NULL) {
		/* Retain the original permanent state before a temporary import hides it. */
		signaled = sync->software_signaled;
		if (!signaled) {
			status = vulkan_sync_status_locked(owner, sync, VULKAN_OPCODE_vkGetFenceStatus);
			if (status == VK_SUCCESS)
				signaled = VK_TRUE;

			if (status == VK_NOT_READY)
				status = VK_SUCCESS;
		}

		if (status == VK_SUCCESS)
			status = external_enable(owner, sync, signaled);
	}

	/* One infallible publication transfers the supplied descriptor after all allocations. */
	retired = -1;
	if (status == VK_SUCCESS) {
		external = sync->external;
		if ((pImportFenceFdInfo->flags & VK_FENCE_IMPORT_TEMPORARY_BIT) != 0) {
			retired = external->temporary;
			external->temporary = pImportFenceFdInfo->fd;
		} else {
			retired = external->permanent;
			external->permanent = pImportFenceFdInfo->fd;
			if (external->temporary >= 0) {
				close(external->temporary);
				external->temporary = -1;
			}
		}

		sync->software_signaled = VK_FALSE;
		sync->notification = 0;
	}

	pthread_mutex_unlock(&owner->mutex);

	/* A failed preparation never transfers the application descriptor to this fence. */
	if (status != VK_SUCCESS)
		return status;

	/* Retire the replaced payload only after the new descriptor belongs to Vulkan. */
	if (retired >= 0)
		close(retired);

	/* Succeeded: the caller must no longer use or close its imported descriptor. */
	return VK_SUCCESS;
}

/*
 * Allocates exportable payloads before exposing a newly created Vulkan fence.
 */
VkResult
vulkan_external_fence_create(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	const VkFenceCreateInfo *create)
{
	const VkExportFenceCreateInfo *chain;
	VkExternalFenceHandleTypeFlags types;
	VkBool32 signaled;
	VkResult status;

	/* The common sType/pNext prefix is sufficient for this flags-only extension. */
	types = 0;
	for (chain = create->pNext; chain != NULL; chain = chain->pNext) {
		if (chain->sType == VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO)
			types = chain->handleTypes;
	}

	if (types == 0)
		return VK_SUCCESS;

	/* Limit advertised semantics to the kernel's reference-bearing OPAQUE payload. */
	if (types != VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT || (device->object.context->capabilities & GPU_CAP_FENCE) == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Native and kernel initial states must agree before the application sees either. */
	signaled = VK_FALSE;
	if ((create->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0)
		signaled = VK_TRUE;

	status = external_enable(device, sync, signaled);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: only explicitly exportable fences may produce OPAQUE descriptors. */
	sync->export_types = types;
	return VK_SUCCESS;
}

/*
 * Observes the shared active payload instead of a stale process-local native fence.
 */
VkResult
vulkan_external_fence_status_locked(
	struct VkDevice_T *device,
	struct vulkan_sync *sync)
{
	struct gpu_fence_state state;
	VkResult status;
	int descriptor;

	/* Kernel identity and generation remain authoritative across all imported aliases. */
	descriptor = external_active(sync->external);
	status = external_query(device, descriptor, &state);
	if (status != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* A dead producer is terminal failure even if this local native fence is healthy. */
	if (state.state == GPU_FENCE_ERROR)
		return VK_ERROR_DEVICE_LOST;

	/* Pending shared work cannot inherit a stale local native signal. */
	if (state.state != GPU_FENCE_SIGNALED)
		return VK_NOT_READY;

	/* Succeeded: verified GPU work or completed acquisition signaled this shared payload. */
	return VK_SUCCESS;
}

/*
 * Waits for the current shared generation without holding Vulkan submission locks.
 */
VkResult
vulkan_external_fence_wait(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	uint64_t timeout_ns)
{
	struct gpu_fence_state state;
	VkResult status;
	int descriptor;
	int error;

	/* External synchronization keeps this fence's active descriptor stable during waiting. */
	descriptor = external_active(sync->external);
	status = external_query(device, descriptor, &state);
	if (status != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* QUERY output fields are not valid inputs to the following state operation. */
	state.state = 0;
	state.error = 0;
	state.timeout_ns = timeout_ns;
	error = ioctl(device->object.context->fd, GPU_FENCE_WAIT, &state);
	if (error != 0) {
		/* Interruption or a concurrent alias reset requires another current-state observation. */
		if (errno == EINTR || errno == ESTALE)
			return VK_SUCCESS;

		/* A caller deadline does not cancel the pending shared generation. */
		if (errno == EAGAIN || errno == ETIMEDOUT)
			return VK_TIMEOUT;

		/* Other kernel failures make this active payload unusable. */
		return VK_ERROR_DEVICE_LOST;
	}

	/* Successful ioctl completion can still carry an explicit producer failure. */
	if (state.state == GPU_FENCE_ERROR)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: the caller reobserves its all-or-any active payload condition. */
	return VK_SUCCESS;
}

/*
 * Restores temporary imports and resets the permanent payload seen by every alias.
 */
VkResult
vulkan_external_fence_reset_locked(
	struct VkDevice_T *device,
	struct vulkan_sync *sync)
{
	struct vulkan_external_fence *external;
	struct gpu_fence_state state;
	VkResult status;
	int error;

	/* Ordinary fences have no shared reference state to restore. */
	external = sync->external;
	if (external == NULL)
		return VK_SUCCESS;

	/* Reset operates on the restored permanent payload, never on the temporary import. */
	status = external_query(device, external->permanent, &state);
	if (status != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* Preserve only fd and generation; terminal observations are output-only ABI fields. */
	state.state = 0;
	state.error = 0;
	error = ioctl(device->object.context->fd, GPU_FENCE_RESET, &state);
	if (error != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Temporary reference retirement cannot affect the restored shared generation. */
	if (external->temporary >= 0) {
		close(external->temporary);
		external->temporary = -1;
	}

	/* Succeeded: a later submit will reset its private native fence before using this payload. */
	return VK_SUCCESS;
}

/*
 * Prepares the exact shared generation before the kernel reserves its GPU job.
 */
VkResult
vulkan_external_fence_prepare_locked(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	int *fd,
	uint64_t *generation,
	VkBool32 prepare_native)
{
	struct gpu_fence_state state;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	int descriptor;

	/* Ordinary fences use the same supervised job without an exported payload. */
	*fd = -1;
	*generation = 0;
	if (sync == NULL || sync->external == NULL)
		return VK_SUCCESS;

	/* Samples the authoritative payload rather than a possibly stale local native fence. */
	descriptor = external_active(sync->external);
	status = external_query(device, descriptor, &state);
	if (status != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* Only an unsignaled generation may be associated with a new native submission. */
	if (state.state != GPU_FENCE_PENDING)
		return VK_ERROR_DEVICE_LOST;

	/* A capacity retry keeps the already prepared native identity while revalidating its shared generation. */
	if (prepare_native == VK_FALSE) {
		*fd = descriptor;
		*generation = state.generation;
		return VK_SUCCESS;
	}

	/* An imported alias may have reset the shared payload without resetting this native object. */
	vulkan_writer_init_for_object(&writer, &sync->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkResetFences);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u32(&writer, 1);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, sync->object.wire_id);
	status = vulkan_command_execute(device->object.context, &writer, 8, &reader, VK_TRUE);
	vulkan_reader_finish(&reader);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: JOB_RESERVE can pin this exact descriptor generation before native acceptance. */
	*fd = descriptor;
	*generation = state.generation;
	return VK_SUCCESS;
}

/*
 * Signals an already completed WSI acquisition through the shared fence payload.
 */
VkResult
vulkan_external_fence_acquire_locked(
	struct VkDevice_T *device,
	struct vulkan_sync *sync)
{
	struct gpu_fence_state state;
	struct gpu_fence_bind bind;
	VkResult status;
	int descriptor;
	int error;

	/* Ordinary acquisition retains its immediately completed software payload. */
	if (sync == NULL || sync->external == NULL)
		return VK_SUCCESS;

	/* Image availability was already verified; no future producer execution is required. */
	descriptor = external_active(sync->external);
	status = external_query(device, descriptor, &state);
	if (status != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* An acquire cannot overwrite an earlier terminal shared payload without reset. */
	if (state.state != GPU_FENCE_PENDING)
		return VK_ERROR_DEVICE_LOST;

	/* Binds only this synchronous operation, which is ineligible for pending GPU dependency waits. */
	memset(&bind, 0, sizeof(bind));
	bind.version = GPU_ABI_VERSION;
	bind.size = sizeof(bind);
	bind.fd = descriptor;
	bind.generation = state.generation;
	error = ioctl(device->object.context->fd, GPU_FENCE_BIND, &bind);
	if (error != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Signals the completed acquisition before returning image ownership to the application. */
	state.state = 0;
	state.error = 0;
	error = ioctl(device->object.context->fd, GPU_FENCE_SIGNAL, &state);
	if (error != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: imported aliases can immediately observe that the image is available. */
	return VK_SUCCESS;
}

/*
 * Borrows the active reference for a level-triggered mixed fence wait.
 */
int
vulkan_external_fence_descriptor(
	struct vulkan_sync *sync)
{
	int descriptor;

	/* External host synchronization retains this descriptor through the caller's wait. */
	descriptor = external_active(sync->external);

	/* Succeeded: poll observes terminal readiness without consuming the shared payload. */
	return descriptor;
}

/*
 * Releases permanent and temporary references after native marker use has retired.
 */
void
vulkan_external_fence_finish(
	struct vulkan_sync *sync)
{
	struct vulkan_external_fence *external;

	/* Partial creation and ordinary fences own no exported descriptors. */
	external = sync->external;
	if (external == NULL)
		return;

	/* Waits only for this process's admitted job, never for an unsubmitted imported payload. */
	vulkan_sync_quiesce(sync);

	/* Temporary imports retain separate ownership until their final local retirement. */
	if (external->temporary >= 0)
		close(external->temporary);

	/* Retires the permanent reference and the exact allocator-owned wrapper. */
	close(external->permanent);
	vulkan_free(&sync->object.allocator, external);
	sync->external = NULL;

	/* Succeeded: other processes retain their independently owned exported references. */
	return;
}

/* Allocates only the wrapper and kernel payload needed for reference-bearing export. */
static VkResult
external_enable(
	struct VkDevice_T *device,
	struct vulkan_sync *sync,
	VkBool32 signaled)
{
	struct vulkan_external_fence *external;
	struct gpu_fence_create create;
	VkResult status;
	int error;

	/* Preserves the fence's effective object allocation callbacks. */
	external = vulkan_allocate(&sync->object.allocator, sizeof(*external), sizeof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (external == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Invalid descriptors distinguish partial construction from acquired references. */
	memset(external, 0, sizeof(*external));
	external->permanent = -1;
	external->temporary = -1;

	/* Initial signaling is a synchronous creation property, independent of queue work. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.flags = GPU_HANDLE_CLOEXEC;
	create.fd = -1;
	create.signaled = signaled;
	error = ioctl(device->object.context->fd, GPU_FENCE_CREATE, &create);
	if (error != 0) {
		status = external_errno(errno);
		vulkan_free(&sync->object.allocator, external);
		return status;
	}

	/* Publishes the complete payload without creating any completion thread. */
	external->permanent = create.fd;
	sync->external = external;

	/* Succeeded: the kernel owns completion of later admitted GPU work. */
	return VK_SUCCESS;
}

/* Selects the temporary payload while preserving the underlying permanent reference. */
static int
external_active(
	struct vulkan_external_fence *external)
{
	/* Temporary imports mask rather than consume the permanent payload. */
	if (external->temporary >= 0)
		return external->temporary;

	/* Succeeded: no temporary reference hides the ordinary permanent payload. */
	return external->permanent;
}

/* Validates descriptor type and GPU identity while snapshotting its authoritative generation. */
static VkResult
external_query(
	struct VkDevice_T *device,
	int fd,
	struct gpu_fence_state *state)
{
	int error;

	/* Generation zero asks the kernel for the current shared payload generation. */
	memset(state, 0, sizeof(*state));
	state->version = GPU_ABI_VERSION;
	state->size = sizeof(*state);
	state->fd = fd;
	error = ioctl(device->object.context->fd, GPU_FENCE_QUERY, state);
	if (error != 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Succeeded: the descriptor belongs to this GPU's current fence ABI. */
	return VK_SUCCESS;
}

/* Maps kernel resource pressure without disguising an invalid external descriptor. */
static VkResult
external_errno(
	int error)
{
	/* File-table and bookkeeping pressure can be retried without losing the device. */
	if (error == ENOMEM || error == EMFILE || error == ENFILE)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Succeeded: preserve the external-handle failure category for unsupported references. */
	return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
