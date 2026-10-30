/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU device of the amdrdna driver: its sessions, resources and
 * sharing (stage A5s).
 *
 * Every session has a GPU address space of its own (space.c).  Storage is
 * plain RAM; a blob is a backing of VRAM in the CPU's window, which the
 * CPU reads and writes through the host data path and maps for userspace
 * as device memory, and which the other opens of the device can import.
 * The device is not unregistered while the machine runs: the GPU keeps
 * the queues the driver started until the reboot, when the shutdown hook
 * takes the device offline before the stages are undone.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-share.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/vm-device.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"

/*
 * The device's capabilities before an executor is bound: storage, VRAM
 * blobs, transfers, CPU mappings, the sharing of blobs, and blobs only the
 * CPU touches in cached RAM.
 */
#define NODE_CAPABILITIES		(GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE | GPU_CAP_HOST_BLOB)

/* The largest resource, and how many one session may hold. */
#define NODE_MAX_RESOURCE_BYTES		(64ULL << 20)
#define NODE_MAX_RESOURCES		1024U

/* The page the storage is allocated in. */
#define NODE_PAGE_BYTES			4096U

static int node_open(void *private_data, void **session);
static void node_close(void *private_data, void *session);
static int node_get_info(void *private_data, void *session, struct gpu_info *info);
static int node_resource_create(void *private_data, void *session, const struct gpu_resource_create *request, void **object);
static void node_resource_destroy(void *private_data, void *session, void *object);
static int node_blob_create(void *private_data, void *session, const struct gpu_blob_create *request, void **object, uint32_t *resource_id);
static int node_host_blob(struct amdrdna_node *node, struct amdrdna_session *session, uint64_t bytes, void **object, uint32_t *resource_id);
static int node_resource_read(void *private_data, void *session, void *object, uint64_t offset, void *buffer, uint32_t bytes);
static int node_resource_write(void *private_data, void *session, void *object, uint64_t offset, const void *buffer, uint32_t bytes);
static int node_resource_map(void *private_data, void *session, void *object, struct drv_gpu_mapping *mapping);
static int node_export(void *private_data, void *session, void *object, const struct gpu_image_descriptor *image, void **result);
static void node_release(void *private_data, void *shared);
static int node_import(void *private_data, void *session, void *shared, uint32_t flags, void **result, uint32_t *resource_id);
static int node_range(const struct amdrdna_resource *resource, uint64_t offset, uint32_t bytes);
static void node_blob_unlisted(struct amdrdna_node *node, struct amdrdna_resource *resource);

/*
 * The sharing operations: export, release and import between the opens
 * of the device; no physical view for another device.  Shared by every
 * node; immutable.
 */
static const struct drv_gpu_share_ops node_share_ops = {
	node_export,
	node_release,
	node_import,
	NULL,
	NULL
};

/*
 * Makes the GPU device's record for stage A5s: its operations and
 * capabilities, the renderer's queue and the recovery operations, and no
 * session yet.  It is registered once the stage's checks passed.
 *
 * Returns 0 or ENOMEM.
 */
int
amdrdna_node_create(
	struct amdrdna_device *device)
{
	struct amdrdna_node *node;
	unsigned vk;

	/* A node made by an earlier try of the stage is kept. */
	if (device->node != NULL)
		return 0;

	/* The node; it lives until power-off. */
	node = kern_calloc(1U, sizeof(*node));
	if (node == NULL)
		return ENOMEM;

	/* The lock and the protocol numbers, from one. */
	(void)mutex_init(&node->lock, LOCK_RANK_DEVICE, "amdrdna node");
	node->device = device;
	node->next_resource_id = 1U;

	/* Describes the device: its interface, its capabilities and every operation. */
	node->ops.version = DRV_GPU_INTERFACE_VERSION;
	node->ops.size = sizeof(node->ops);
	node->ops.capabilities = NODE_CAPABILITIES;
	node->ops.open = node_open;
	node->ops.close = node_close;
	node->ops.get_info = node_get_info;
	node->ops.resource_create = node_resource_create;
	node->ops.resource_destroy = node_resource_destroy;
	node->ops.blob_create = node_blob_create;
	node->ops.resource_read = node_resource_read;
	node->ops.resource_write = node_resource_write;
	node->ops.resource_map = node_resource_map;
	node->ops.share = &node_share_ops;

	/* The renderer's queue and the recovery operations (the stop of a session, the fault). */
	amdrdna_runtime_prepare(node);

	/* The Vulkan executor makes the device a renderer (before it is registered), unless the boot says amdrdna.vk=0. */
	vk = amdrdna_parameter_number("amdrdna.vk", 1U);
	if (vk != 0U)
		amdrdna_vk_node_bind(node);

	/* Succeeded: the node is the device's. */
	device->node = node;
	return 0;
}

/*
 * Registers the GPU device with the GPU core, which publishes /dev/gpuN,
 * and starts the renderer's worker when an executor is bound.
 *
 * Returns 0, or the core's error (nothing is registered then).
 */
int
amdrdna_node_register(
	struct amdrdna_device *device)
{
	struct amdrdna_node *node;
	int error;

	/* The node made earlier by the stage. */
	node = device->node;
	if (node == NULL)
		return ENODEV;
	if (node->gpu != NULL)
		return 0;

	/* Registers the device; the core publishes /dev/gpuN. */
	error = drv_gpu_register(&node->ops, node, &node->gpu);
	if (error != 0)
		return error;

	/* The renderer's worker runs once the device is registered. */
	if (node->renderer != 0)
		amdrdna_runtime_start(node);

	/* Succeeded: the GPU is a GPU device. */
	return 0;
}

/*
 * Stops the GPU device for the reboot or the shutdown of the machine,
 * before the stages are undone: the display's lease ends, the renderer
 * takes no more work, no address space runs anything new, and the device
 * goes offline (opens
 * that remain keep the core's registration, which the reset makes moot).
 */
void
amdrdna_node_stop(
	struct amdrdna_device *device)
{
	struct amdrdna_node *node;
	int error;

	/* Nothing registered, nothing to stop. */
	node = device->node;
	if (node == NULL)
		return;

	/* The display's lease ends and nothing touches the display after. */
	amdrdna_present_stop(node);

	/* No queued frame and no address space runs anything new. */
	amdrdna_runtime_stop(node);
	amdrdna_spaces_fault(device);

	/* The device goes offline. */
	if (node->gpu == NULL)
		return;
	error = drv_gpu_unregister(node->gpu);
	if (error == 0) {
		node->gpu = NULL;
	} else if (error == EBUSY) {
		amdrdna_mark("shutdown: the GPU device is offline; opens remain until the reset");
	} else {
		amdrdna_mark("shutdown: the GPU device was not withdrawn (%d)", error);
	}
}

/*
 * Tells the GPU core that one session lost its work: its later
 * operations fail, the other sessions go on.  A session the core does not
 * know (one of the driver's own checks) is ignored by the core.
 */
void
amdrdna_node_session_lost(
	struct amdrdna_device *device,
	void *session,
	int error)
{
	struct amdrdna_node *node;
	struct drv_gpu_device *gpu;

	/* Without a registered device nobody is told. */
	node = device->node;
	if (node == NULL)
		return;
	gpu = node->gpu;
	if (gpu == NULL)
		return;

	/* The core marks the session failed. */
	drv_gpu_report_session_error(gpu, session, error);
}

/*
 * Tells the GPU core that the device failed (a fault the driver found
 * itself): every session's work fails from now on.  Without a registered
 * device the address spaces stop taking runs.
 */
void
amdrdna_node_device_failed(
	struct amdrdna_device *device,
	int error)
{
	struct amdrdna_node *node;

	/* No new run, whoever is told. */
	amdrdna_spaces_fault(device);

	/* The core faults the device and calls back. */
	node = device->node;
	if (node == NULL || node->gpu == NULL)
		return;
	drv_gpu_report_error(node->gpu, error);
}

/*
 * Opens a session of the node: its record and its address space.  The
 * core's open and the driver's own checks open sessions the same way.
 *
 * Returns 0, EIO once the device failed, ENOSPC, ENOMEM.
 */
int
amdrdna_node_session_open(
	struct amdrdna_node *node,
	struct amdrdna_session **result)
{
	struct amdrdna_session *opened;
	int error;

	/* A failed device takes no new session. */
	*result = NULL;
	if (node->faulted != 0)
		return EIO;

	/* The record. */
	opened = kern_calloc(1U, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;

	/* Its address space, which reports a loss under the session's name. */
	error = amdrdna_space_create(node->device, opened, &opened->space);
	if (error != 0) {
		kern_free(opened);
		return error;
	}

	/* Succeeded: the session is the caller's. */
	opened->node = node;
	*result = opened;
	return 0;
}

/*
 * Closes a session of the node: its address space goes (or is kept with
 * its tables when its work could not be confirmed ended), then its record.
 */
void
amdrdna_node_session_close(
	struct amdrdna_node *node,
	struct amdrdna_session *session)
{
	bool kept;

	UNUSED_PARAMETER(node);

	/* The address space; a kept one stays the device's. */
	kept = amdrdna_space_destroy(session->space);
	if (kept)
		amdrdna_mark("gpu device: a session's address space is kept until the reset");

	/* The record. */
	kern_free(session);
}

/* Numbers a new blob or import for the protocol: never zero. */
uint32_t
amdrdna_node_resource_id(
	struct amdrdna_node *node)
{
	uint32_t resource_id;

	/* Takes the next number, skipping zero when the count wraps. */
	mutex_lock(&node->lock);

	resource_id = node->next_resource_id;
	node->next_resource_id++;
	if (node->next_resource_id == 0U)
		node->next_resource_id = 1U;

	mutex_unlock(&node->lock);

	/* Succeeded: the number is the resource's. */
	return resource_id;
}

/* Lists a blob in its session under its protocol number, so the executor finds it. */
void
amdrdna_node_blob_listed(
	struct amdrdna_node *node,
	struct amdrdna_session *session,
	struct amdrdna_resource *resource,
	uint32_t resource_id)
{
	/* At the head of the session's list. */
	mutex_lock(&node->lock);

	resource->resource_id = resource_id;
	resource->session = session;
	resource->next_blob = session->blobs;
	session->blobs = resource;

	mutex_unlock(&node->lock);
}

/* Opens a session for the GPU core. */
static int
node_open(
	void *private_data,
	void **session)
{
	struct amdrdna_session *opened;
	int error;

	/* The session and its address space. */
	error = amdrdna_node_session_open(private_data, &opened);
	if (error != 0)
		return error;

	/* Its executor session when the device is a renderer. */
	error = amdrdna_vk_node_open(opened);
	if (error != 0) {
		amdrdna_node_session_close(private_data, opened);
		return error;
	}

	/* Succeeded: the session is the core's. */
	*session = opened;
	return 0;
}

/* Closes a session the GPU core opened; its resources were destroyed before. */
static void
node_close(
	void *private_data,
	void *session)
{
	int error;

	/* The session's display lease ends, bringing the console back. */
	amdrdna_present_session_close(private_data, session);

	/* The executor session first: it gives back its memory in the address space. */
	error = amdrdna_vk_node_close(session);
	if (error != 0)
		return;

	/* The address space and the record. */
	amdrdna_node_session_close(private_data, session);
}

/* Describes the device to userspace. */
static int
node_get_info(
	void *private_data,
	void *session,
	struct gpu_info *info)
{
	struct amdrdna_node *node;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;

	/* The capabilities (the renderer's too once bound) and the limits of the resources. */
	info->capabilities = node->ops.capabilities;
	info->max_resources = NODE_MAX_RESOURCES;
	info->max_resource_bytes = NODE_MAX_RESOURCE_BYTES;

	/* Names the driver, with the terminating NUL. */
	kern_memset(info->driver_name, 0, sizeof(info->driver_name));
	kern_memcpy(info->driver_name, "amdrdna", 8U);

	/* Succeeded: userspace can identify the device. */
	return 0;
}

/* Creates ordinary storage: cleared, contiguous RAM. */
static int
node_resource_create(
	void *private_data,
	void *session,
	const struct gpu_resource_create *request,
	void **object)
{
	struct amdrdna_resource *resource;
	uint64_t size;
	int error;

	UNUSED_PARAMETER(private_data);
	UNUSED_PARAMETER(session);

	/* The core bounded the size; the run is whole pages. */
	if (request->bytes == 0U || request->bytes > NODE_MAX_RESOURCE_BYTES)
		return EINVAL;
	size = (request->bytes + NODE_PAGE_BYTES - 1U) & ~((uint64_t)NODE_PAGE_BYTES - 1U);

	/* The record. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* The RAM. */
	error = kern_pmem_alloc((size_t)size, NODE_PAGE_BYTES, &resource->ram);
	if (error != 0) {
		kern_free(resource);
		return ENOMEM;
	}

	/* Cleared through the direct map. */
	resource->cpu = kern_pmem_to_kernel(resource->ram.paddr);
	if (resource->cpu == NULL) {
		(void)kern_pmem_free(&resource->ram);
		kern_free(resource);
		return EIO;
	}

	/* Clears the RAM and describes the storage. */
	kern_memset(resource->cpu, 0, (size_t)size);
	resource->kind = AMDRDNA_RESOURCE_STORAGE;
	resource->bytes = request->bytes;

	/* Succeeded: the storage is the session's. */
	*object = resource;
	return 0;
}

/*
 * Destroys a resource of either kind.  A blob leaves its session's list
 * and drops its reference to the backing; when the session's address
 * space may still run work, the backing's VRAM is kept for good.
 */
static void
node_resource_destroy(
	void *private_data,
	void *session,
	void *object)
{
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	struct amdrdna_resource *resource;
	bool quiet;

	/* The node and what the core passed in. */
	node = private_data;
	owner = session;
	resource = object;

	/* Storage (a blob only the CPU touches leaves its session's list first): its RAM goes back. */
	if (resource->kind == AMDRDNA_RESOURCE_STORAGE) {
		node_blob_unlisted(node, resource);
		(void)kern_pmem_free(&resource->ram);
		kern_free(resource);
		return;
	}

	/* A blob on the screen goes off it first. */
	amdrdna_present_resource_gone(node, resource);

	/* A blob leaves its session's list. */
	node_blob_unlisted(node, resource);

	/* Work of the session that may still run, or a session isolated, keeps the VRAM. */
	quiet = amdrdna_space_quiet(owner->space);
	if (!quiet || owner->isolated != 0)
		amdrdna_backing_keep(node->device, resource->backing);

	/* Drops the reference; the last one gives the VRAM back. */
	amdrdna_backing_put(node->device, resource->backing);

	/* The record. */
	kern_free(resource);
}

/*
 * Creates a blob: a backing of VRAM, cleared, which the session and the
 * opens it is shared with read and write; in the CPU's window when it is
 * to be mapped, else in the VRAM only the GPU reaches.
 */
static int
node_blob_create(
	void *private_data,
	void *session,
	const struct gpu_blob_create *request,
	void **object,
	uint32_t *resource_id)
{
	struct amdrdna_node *node;
	struct amdrdna_resource *resource;
	struct amdrdna_backing *backing;
	int error;

	/* The node and what the core passed in. */
	node = private_data;

	/* The core bounded the size. */
	if (request->bytes == 0U || request->bytes > NODE_MAX_RESOURCE_BYTES)
		return EINVAL;

	/* A blob only the CPU touches, not a VkDeviceMemory's storage, is cached RAM. */
	if ((request->flags & GPU_BLOB_HOST) != 0U && request->blob_id == 0U) {
		error = node_host_blob(node, session, request->bytes, object, resource_id);
		if (error != 0)
			return error;

		/* Succeeded: the blob is the session's, in RAM. */
		return 0;
	}

	/* The record. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* The backing, cleared, in the window only when the blob is mapped. */
	error = amdrdna_backing_create(node->device, request->bytes, (request->flags & GPU_BLOB_MAPPABLE) != 0U, &backing);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* The blob holds the backing's one reference. */
	resource->kind = AMDRDNA_RESOURCE_BLOB;
	resource->bytes = request->bytes;
	resource->backing = backing;

	/*
	 * A blob of the renderer's made for a VkDeviceMemory (its blob_id is
	 * the memory's identity) is that memory's storage: the memory takes a
	 * reference to the backing and the session's address space maps it.
	 */
	if (request->blob_id != 0U && node->renderer != 0) {
		error = amdrdna_vk_node_blob_attach(session, request->blob_id, backing, request->bytes);
		if (error != 0) {
			amdrdna_backing_put(node->device, backing);
			kern_free(resource);
			return error;
		}
	}

	/* Succeeded: the blob is the session's, numbered for the protocol and listed. */
	*resource_id = amdrdna_node_resource_id(node);
	amdrdna_node_blob_listed(node, session, resource, *resource_id);
	*object = resource;
	return 0;
}

/*
 * Creates a blob only the CPU touches: cleared, contiguous RAM the process
 * maps cached and the executor reads directly, numbered and listed in the
 * session as a blob is.  Returns 0, ENOMEM or EIO.
 */
static int
node_host_blob(
	struct amdrdna_node *node,
	struct amdrdna_session *session,
	uint64_t bytes,
	void **object,
	uint32_t *resource_id)
{
	struct amdrdna_resource *resource;
	uint64_t size;
	int error;

	/* The run is whole pages. */
	size = (bytes + NODE_PAGE_BYTES - 1U) & ~((uint64_t)NODE_PAGE_BYTES - 1U);

	/* The record. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* The RAM. */
	error = kern_pmem_alloc((size_t)size, NODE_PAGE_BYTES, &resource->ram);
	if (error != 0) {
		kern_free(resource);
		return ENOMEM;
	}

	/* Its view through the direct map. */
	resource->cpu = kern_pmem_to_kernel(resource->ram.paddr);
	if (resource->cpu == NULL) {
		(void)kern_pmem_free(&resource->ram);
		kern_free(resource);
		return EIO;
	}

	/* Cleared, and described as storage. */
	kern_memset(resource->cpu, 0, (size_t)size);
	resource->kind = AMDRDNA_RESOURCE_STORAGE;
	resource->bytes = bytes;

	/* Succeeded: numbered for the protocol and listed, so the executor finds it. */
	*resource_id = amdrdna_node_resource_id(node);
	amdrdna_node_blob_listed(node, session, resource, *resource_id);
	*object = resource;
	return 0;
}

/* Copies bytes of a resource out to a kernel buffer. */
static int
node_resource_read(
	void *private_data,
	void *session,
	void *object,
	uint64_t offset,
	void *buffer,
	uint32_t bytes)
{
	struct amdrdna_node *node;
	struct amdrdna_resource *resource;
	volatile uint8_t *source;
	uint8_t *target;
	uint32_t at;
	int error;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;
	resource = object;
	target = buffer;

	/* The range lies inside the resource. */
	error = node_range(resource, offset, bytes);
	if (error != 0)
		return error;

	/* Storage is plain RAM. */
	if (resource->kind == AMDRDNA_RESOURCE_STORAGE) {
		kern_memcpy(target, resource->cpu + offset, bytes);
		return 0;
	}

	/* A blob outside the window is read a word at a time through the indirect window, whole words only. */
	if (!resource->backing->cpu) {
		if ((offset % 4U) != 0U || (bytes % 4U) != 0U)
			return EINVAL;
		amdrdna_vram_read(node->device, resource->backing->vram + offset, buffer, bytes);
		return 0;
	}

	/* A blob is read through the window, after the host path's read cache is dropped. */
	amdrdna_host_path_drop(node->device);
	source = amdrdna_backing_cpu(node->device, resource->backing) + offset;
	for (at = 0U; at < bytes; at++)
		target[at] = source[at];

	/* Succeeded: the bytes are in the buffer. */
	return 0;
}

/* Copies bytes of a kernel buffer into a resource. */
static int
node_resource_write(
	void *private_data,
	void *session,
	void *object,
	uint64_t offset,
	const void *buffer,
	uint32_t bytes)
{
	struct amdrdna_node *node;
	struct amdrdna_resource *resource;
	volatile uint8_t *target;
	const uint8_t *source;
	uint32_t at;
	int error;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;
	resource = object;
	source = buffer;

	/* The range lies inside the resource. */
	error = node_range(resource, offset, bytes);
	if (error != 0)
		return error;

	/* Storage is plain RAM. */
	if (resource->kind == AMDRDNA_RESOURCE_STORAGE) {
		kern_memcpy(resource->cpu + offset, source, bytes);
		return 0;
	}

	/* A blob outside the window is written a word at a time through the indirect window, whole words only. */
	if (!resource->backing->cpu) {
		if ((offset % 4U) != 0U || (bytes % 4U) != 0U)
			return EINVAL;
		amdrdna_vram_write(node->device, resource->backing->vram + offset, buffer, bytes);
		return 0;
	}

	/* A blob is written through the window, read back once so the writes have landed. */
	target = amdrdna_backing_cpu(node->device, resource->backing) + offset;
	for (at = 0U; at < bytes; at++)
		target[at] = source[at];
	if (bytes != 0U)
		(void)target[bytes - 1U];

	/* The GPU and the display read what was written, not what the host path holds. */
	amdrdna_host_path_flush(node->device);

	/* Succeeded: the bytes are in the resource. */
	return 0;
}

/* Describes the CPU view of a resource: cached RAM for storage, the window (device memory) for a blob. */
static int
node_resource_map(
	void *private_data,
	void *session,
	void *object,
	struct drv_gpu_mapping *mapping)
{
	struct amdrdna_node *node;
	struct amdrdna_resource *resource;
	bool write_combine;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;
	resource = object;

	/* Storage: the run of RAM. */
	if (resource->kind == AMDRDNA_RESOURCE_STORAGE) {
		mapping->physical = resource->ram.paddr;
		mapping->address = resource->cpu;
		mapping->bytes = resource->ram.size;
		mapping->attributes = 0U;
		return 0;
	}

	/* A blob outside the window has no CPU view. */
	if (!resource->backing->cpu)
		return EINVAL;

	/*
	 * A blob: its place in the window, the pages it was asked for (the
	 * backing is whole runs of the address spaces' 64 KiB; libvulkan maps
	 * exactly the pages it asked for).
	 */
	mapping->physical = amdrdna_vram_bus(node->device, resource->backing->vram);
	mapping->address = (void *)(uintptr_t)amdrdna_backing_cpu(node->device, resource->backing);
	mapping->bytes = (resource->bytes + NODE_PAGE_BYTES - 1U) & ~((uint64_t)NODE_PAGE_BYTES - 1U);
	mapping->attributes = DRV_GPU_MAPPING_DEVICE;

	/*
	 * The application's writes into the window combine where the kernel
	 * takes the policy (amd64; another port refuses it): the host-visible
	 * memory is written far more than read, and the submission's locked
	 * instructions drain the combining buffers before the GPU reads.
	 *
	 * Procedure referenced: Linux 6.12 drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c
	 * and amdgpu_bo.c (visible VRAM mapped write-combined for the CPU).
	 */
	write_combine = vm_device_write_combine_supported();
	if (write_combine)
		mapping->attributes |= DRV_GPU_MAPPING_WRITE_COMBINE;

	/* Succeeded: the view covers the blob's pages. */
	return 0;
}

/*
 * Exports a blob to the other opens of the device: the export is one more
 * reference to the blob's backing, which the core releases with
 * node_release().
 */
static int
node_export(
	void *private_data,
	void *session,
	void *object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct amdrdna_node *node;
	struct amdrdna_resource *resource;

	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(image);

	/* The node and what the core passed in. */
	node = private_data;
	resource = object;

	/* Only a blob has VRAM to share. */
	if (resource->kind != AMDRDNA_RESOURCE_BLOB)
		return EINVAL;

	/* The export holds the backing as a blob does. */
	amdrdna_backing_get(node->device, resource->backing);

	/* Succeeded: the core holds the backing until it releases the export. */
	*result = resource->backing;
	return 0;
}

/* Releases one export of a blob's backing; the last reference gives the VRAM back. */
static void
node_release(
	void *private_data,
	void *shared)
{
	struct amdrdna_node *node;

	/* The node and what the core passed in. */
	node = private_data;

	/* Drops the export's reference. */
	amdrdna_backing_put(node->device, shared);
}

/*
 * Imports an exported backing into a session as a blob of its own, which
 * names the same VRAM and is destroyed like any blob.
 */
static int
node_import(
	void *private_data,
	void *session,
	void *shared,
	uint32_t flags,
	void **result,
	uint32_t *resource_id)
{
	struct amdrdna_node *node;
	struct amdrdna_backing *backing;
	struct amdrdna_resource *resource;

	(void)flags;

	/* The node and what the core passed in. */
	node = private_data;
	backing = shared;
	*result = NULL;
	*resource_id = 0U;

	/* The importing session's blob. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* The blob holds one more reference to the backing (the export's keeps it alive meanwhile). */
	amdrdna_backing_get(node->device, backing);

	/* Describes the blob: the backing's size, as its exporter asked for it. */
	resource->kind = AMDRDNA_RESOURCE_BLOB;
	resource->bytes = backing->asked;
	resource->backing = backing;

	/* Succeeded: the session has the blob, numbered for the protocol and listed. */
	*resource_id = amdrdna_node_resource_id(node);
	amdrdna_node_blob_listed(node, session, resource, *resource_id);
	*result = resource;
	return 0;
}

/* Reports whether a range of bytes lies inside a resource: EINVAL otherwise. */
static int
node_range(
	const struct amdrdna_resource *resource,
	uint64_t offset,
	uint32_t bytes)
{
	/* The offset is inside, and the bytes after it too. */
	if (offset > resource->bytes)
		return EINVAL;
	if (bytes > resource->bytes - offset)
		return EINVAL;

	/* The range is inside. */
	return 0;
}

/* Takes a blob out of its session's list (a blob never listed is left alone). */
static void
node_blob_unlisted(
	struct amdrdna_node *node,
	struct amdrdna_resource *resource)
{
	struct amdrdna_resource **link;

	/* Unlinks it from the session's list. */
	mutex_lock(&node->lock);

	if (resource->session != NULL) {
		for (link = &resource->session->blobs; *link != NULL; link = &(*link)->next_blob) {
			if (*link == resource) {
				*link = resource->next_blob;
				break;
			}
		}
	}

	/* The blob is no session's any more. */
	resource->session = NULL;
	resource->next_blob = NULL;

	mutex_unlock(&node->lock);
}
