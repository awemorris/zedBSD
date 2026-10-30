/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU device of the nvrtx driver: sessions and resources (the gpu-device
 * stage).
 *
 * By default (unless nvrtx.resident=0) with a head taken over (takeover), the
 * worker leaves the GSP up and registers the GPU with the GPU core.  The
 * device has ordinary storage (RAM the CPU reads and writes), blobs in the
 * VRAM (buffer objects of vram-window's allocator, mapped in the CPU's window,
 * shared between opens by share.c), transfers and CPU mappings of both, and
 * the display of present.c and cursor.c.  With nvrtx.vk=1 it is also a
 * renderer (runtime.c: the Vulkan protocol's commands).
 *
 * The device is not unregistered while the machine runs: the GSP has no
 * path back to the firmware while it runs, so the GPU is kept until the
 * reboot or the shutdown (detach refuses).  Then the PCI shutdown hook
 * stops the device (nvrtx_node_stop: it goes offline, the lease ends, the
 * drain stops) and the GSP (unload), so the next boot finds the GPU as the
 * firmware left it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/vm-device.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-allocation.h>
#include <uapi/gpu-display.h>

#include "drivers/gpu/nvrtx/nvrtx-node.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"

/*
 * The device's capabilities: storage, VRAM blobs, transfers, CPU mappings,
 * sharing of blobs (as images, and as plain allocations such as a decoded
 * picture another open samples), the display with its events and its
 * control, and blobs only the CPU touches in cached RAM (a transport's
 * streams and replies, which the executor reads and writes).
 */
#define NODE_CAPABILITIES		(GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE | \
					 GPU_CAP_ALLOCATION_SHARE | GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS | GPU_CAP_DISPLAY_CONTROL | \
					 GPU_CAP_HOST_BLOB)

/* The largest resource, and how many one session may hold. */
#define NODE_MAX_RESOURCE_BYTES		(64ULL << 20)
#define NODE_MAX_RESOURCES		1024U

/* A blob's VRAM is on this boundary (a window's surface address is a multiple of 256). */
#define NODE_BLOB_ALIGN			0x10000ULL

/*
 * How long each turn of the message drain takes messages (it holds the
 * GSP's lock through a turn, so a call waits at most this long), its pause
 * after a failed turn, its yield to a thread waiting for the lock (at least
 * a scheduler tick) and how often it looks at the refresh count.
 */
#define NODE_DRAIN_TURN_US		20000U
#define NODE_DRAIN_PAUSE_US		1000000U
#define NODE_DRAIN_YIELD_US		1000U
#define NODE_DRAIN_SAMPLE_US		1000000U

/* How often the drain writes the counts nvrtx.vk.perf=1 asks for. */
#define NODE_PERF_PERIOD_US		5000000U

/* Matches no function: the drain takes every message until its turn ends. */
#define NODE_DRAIN_NO_FUNCTION		0xffffffffU

/* The page the storage is allocated in. */
#define NODE_PAGE_BYTES			4096U

/* How long the stop waits for the message drain to end (a turn, its pause and a margin), and its step. */
#define NODE_DRAIN_STOP_TIMEOUT_US	3500000U
#define NODE_DRAIN_STOP_STEP_US		10000U

static int node_open(void *private_data, void **session);
static void node_close(void *private_data, void *session);
static int node_get_info(void *private_data, void *session, struct gpu_info *info);
static int node_resource_create(void *private_data, void *session, const struct gpu_resource_create *request, void **object);
static void node_resource_destroy(void *private_data, void *session, void *object);
static void node_blob_unlisted(struct nvrtx_node *node, struct nvrtx_resource *resource);
static int node_blob_create(void *private_data, void *session, const struct gpu_blob_create *request, void **object, uint32_t *resource_id);
static int node_host_blob(struct nvrtx_node *node, struct nvrtx_session *session, uint64_t bytes, void **object, uint32_t *resource_id);
static int node_resource_read(void *private_data, void *session, void *object, uint64_t offset, void *buffer, uint32_t bytes);
static int node_resource_write(void *private_data, void *session, void *object, uint64_t offset, const void *buffer, uint32_t bytes);
static int node_resource_map(void *private_data, void *session, void *object, struct drv_gpu_mapping *mapping);
static int node_range(const struct nvrtx_resource *resource, uint64_t offset, uint32_t bytes);
static void node_free(struct nvrtx_node *node);
static void node_drain(void *argument);
static void node_perf_report(struct nvrtx_node *node);
static uint64_t node_perf_average(uint64_t total, uint64_t count);

/*
 * Registers the GPU as a GPU device after the takeover (the gpu-device stage).
 *
 * Needs the CPU's window (vram-window) and the head taken over (takeover).
 * Returns 0 once the device is registered: the GSP must then stay up.  On a
 * failure nothing is registered and the caller stops the GSP as usual.
 */
int
nvrtx_node_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_node *node;
	struct thread *drain;
	bool vk_wanted;
	int error;

	/* The display's frames need the window and a head of the driver's own. */
	nvrtx_stage_mark("gpu-device begin");
	if (gpu->memory_state.bar1_ready == 0) {
		nvrtx_stage_mark("gpu-device fail: no bar1 window");
		return ENODEV;
	}

	if (gpu->display.window_made == 0) {
		nvrtx_stage_mark("gpu-device fail: no head taken over");
		return ENODEV;
	}

	/* Allocates the node; it lives until power-off. */
	node = kern_calloc(1U, sizeof(*node));
	if (node == NULL)
		return ENOMEM;

	/* Initializes the node: no lease, numbers from one, the refresh count from one. */
	(void)mutex_init(&node->present_lock, LOCK_RANK_DEVICE, "nvrtx present");
	(void)mutex_init(&node->lock, LOCK_RANK_DEVICE, "nvrtx node");
	spin_init(&node->refresh_lock, LOCK_RANK_DEVICE, "nvrtx refresh");
	node->gpu = gpu;
	node->next_lease = 1U;
	node->next_resource_id = 1U;
	node->refresh_count = 1U;

	/* The counts every 5 s, when the boot asked for them. */
	node->perf = 0;
	if (nvrtx_stage_vk_perf_wanted())
		node->perf = 1;
	node->perf_since_us = nvrtx_now_us();

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
	node->ops.display = &nvrtx_display_ops;
	node->ops.share = &nvrtx_share_ops;
	node->ops.scanout = &nvrtx_scanout_ops;

	/* With nvrtx.vk=1 the device is also a renderer. */
	vk_wanted = nvrtx_stage_vk_wanted();
	if (vk_wanted)
		nvrtx_runtime_bind(node);

	/* Takes the display's frames and shows the console surface. */
	error = nvrtx_present_start(node);
	if (error != 0) {
		nvrtx_stage_mark("gpu-device fail: the display's frames (%d)", error);
		node_free(node);
		return error;
	}

	/*
	 * Makes the cursor plane while the worker is still the GSP's only
	 * caller (its channel is made through the resource manager); without
	 * it the display has no cursor plane.
	 */
	nvrtx_cursor_start(node);

	/* Registers the device; the core publishes /dev/gpuN. */
	error = drv_gpu_register(&node->ops, node, &node->device);
	if (error != 0) {
		nvrtx_stage_mark("gpu-device fail: register (%d)", error);
		node_free(node);
		return error;
	}

	/* The renderer's worker runs once the device is registered. */
	if (node->renderer != 0)
		nvrtx_runtime_start(node);

	/*
	 * Starts the thread that takes the messages the GSP sends meanwhile
	 * (channel errors, display events), so its queue never fills.  Without
	 * it the device still works for a while, so a failure is only shown.
	 *
	 * Procedure referenced: Linux v6.19
	 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
	 * (r535_gsp_msgq_work).
	 */
	error = kthread_create(node_drain, node, SCHED_PRIORITY_DEFAULT, &drain);
	if (error == 0) {
		node->drain_running = 1;
		drain->detached = 1;
		thread_start(drain);
	} else {
		nvrtx_stage_mark("gpu-device: no message drain (%d)", error);
	}

	/* Succeeded: the GPU is a GPU device; the GSP stays up for it. */
	gpu->node = node;
	nvrtx_stage_mark("gpu-device ok: gpu device registered, display %ux%u at %u mHz",
	    (unsigned)node->width,
	    (unsigned)node->height,
	    (unsigned)node->refresh_millihz);
	return 0;
}

/*
 * Stops the GPU device and the GSP for the reboot or the shutdown of the
 * machine.
 *
 * In this order: the renderer takes no more work; the device goes offline
 * (the core refuses new opens and operations; an open that remains only
 * delays the registration's release, which the reset makes moot); the
 * message drain ends, so this thread is the GSP's only reader; the lease
 * ends under the node's present lock and its lock, which bring the
 * firmware's picture back on the head and are held while the GSP is
 * stopped (unload), so no presentation or frame runs meanwhile; the display is
 * broken after it.  Every step is bounded and a failure is only shown.
 */
void
nvrtx_node_stop(
	struct nvrtx_node *node)
{
	uint64_t start;
	uint64_t waited;
	int error;

	/* The renderer refuses new frames and markers from here on. */
	nvrtx_runtime_stop(node);

	/* The device goes offline; opens that remain keep the registration (offline too), which the reset ends. */
	error = drv_gpu_unregister(node->device);
	if (error == 0) {
		node->device = NULL;
	} else if (error == EBUSY) {
		nvrtx_stage_mark("unload shutdown: the GPU device is offline; opens remain until the reset");
	} else {
		nvrtx_stage_mark("unload shutdown: the GPU device was not withdrawn (%d)", error);
	}

	/* Ends the message drain at its next turn, so the GSP's replies reach the stop. */
	node->drain_stop = 1;
	start = nvrtx_now_us();
	for (;;) {
		if (node->drain_running == 0)
			break;

		waited = nvrtx_now_us() - start;
		if (waited > NODE_DRAIN_STOP_TIMEOUT_US) {
			nvrtx_stage_mark("unload shutdown: the message drain did not end in %u ms", (unsigned)(waited / 1000U));
			break;
		}

		kern_usleep_range(NODE_DRAIN_STOP_STEP_US, NODE_DRAIN_STOP_STEP_US * 2U);
	}

	/*
	 * Brings the firmware's picture back and stops the GSP while nothing
	 * else may use the display or the channel: a presentation still
	 * waiting for its flip finishes first (the present lock).  The display
	 * is broken from then on, so a presentation or a cursor change that
	 * comes after touches nothing of the stopped GPU.
	 */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->lock);

	nvrtx_present_stop(node);
	(void)nvrtx_gsp_shutdown(node->gpu);
	node->broken = 1;

	mutex_unlock(&node->lock);
	mutex_unlock(&node->present_lock);
}

/*
 * Drops one reference to a blob's VRAM: the last one gives the VRAM back,
 * unless the display may still read it (kept), and frees the record.
 */
void
nvrtx_node_backing_put(
	struct nvrtx_node *node,
	struct nvrtx_backing *backing)
{
	uint32_t refs;

	/* Counts the reference off under the node's lock. */
	mutex_lock(&node->lock);

	backing->refs--;
	refs = backing->refs;

	mutex_unlock(&node->lock);

	/* Others still name the VRAM. */
	if (refs != 0U)
		return;

	/* The VRAM and its place in the window go back, unless the display may still read it. */
	if (backing->kept == 0)
		nvrtx_bo_free(node->gpu, &backing->bo);

	/* Frees the record. */
	kern_free(backing);
}

/*
 * Gives the CPU's view of a blob's VRAM, mapping it into the window the first
 * time the CPU reaches it; it then stays mapped until the VRAM is given back.
 *
 * Returns ENODEV without a working window, or ENOSPC when the window is full.
 */
int
nvrtx_node_backing_cpu(
	struct nvrtx_node *node,
	struct nvrtx_backing *backing,
	volatile uint8_t **cpu)
{
	int error;

	/* The window's place (nvrtx_bo_map keeps one it has). */
	error = nvrtx_bo_map(node->gpu, &backing->bo);
	if (error != 0)
		return error;

	/* Succeeded: the blob's first byte in the window. */
	*cpu = nvrtx_bo_cpu(node->gpu, &backing->bo);
	return 0;
}

/*
 * Numbers a new blob or import for the protocol: never zero.
 */
uint32_t
nvrtx_node_resource_id(
	struct nvrtx_node *node)
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

/*
 * Lists a blob in its session under its protocol number, so the executor
 * finds it.
 */
void
nvrtx_node_blob_listed(
	struct nvrtx_node *node,
	struct nvrtx_session *session,
	struct nvrtx_resource *resource,
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

/*
 * Gives the CPU view of a blob of the executor's owner (a node session) by
 * its protocol number.  The caller holds the node's lock.
 */
int
nvrtx_vk_blob(
	void *owner,
	uint32_t resource_id,
	uint8_t **cpu,
	uint64_t *bytes)
{
	struct nvrtx_session *session;
	struct nvrtx_resource *resource;
	volatile uint8_t *view;
	int error;

	session = owner;

	/* Finds the number among the session's blobs. */
	for (resource = session->blobs; resource != NULL; resource = resource->next_blob) {
		if (resource->resource_id == resource_id)
			break;
	}

	/* Refuses a number the session has no blob for. */
	if (resource == NULL)
		return ENOENT;

	/* A blob only the CPU touches is read in its RAM, through the CPU's cache. */
	if (resource->kind == NVRTX_RESOURCE_STORAGE) {
		*cpu = resource->cpu;
		*bytes = resource->bytes;
		return 0;
	}

	/* The blob in the CPU's window (taken now if it has no place yet). */
	error = nvrtx_node_backing_cpu(session->node, resource->backing, &view);
	if (error != 0)
		return error;

	/* Succeeded: the blob's bytes in the CPU's window and their count. */
	*cpu = (uint8_t *)(uintptr_t)view;
	*bytes = resource->bytes;
	return 0;
}

/*
 * Gives the executor's owner's GPU's PCI product.
 */
uint32_t
nvrtx_vk_device_id(
	void *owner)
{
	struct nvrtx_session *session;

	session = owner;

	/* The product the attach read. */
	return session->node->gpu->identity.product;
}

/*
 * Gives the bytes of VRAM the executor's memory may take: what the CPU's
 * window holds after the console's part.
 */
uint64_t
nvrtx_vk_heap_bytes(
	void *owner)
{
	struct nvrtx_session *session;
	const struct nvrtx_memory_state *memory;

	session = owner;
	memory = &session->node->gpu->memory_state;

	/* The window's part the blobs are mapped in. */
	return memory->bar1_window - memory->bar1_cpu_from;
}

/*
 * Gives the shader model the executor compiles the owner's programs for.
 */
uint32_t
nvrtx_vk_shader_model(
	void *owner)
{
	struct nvrtx_session *session;

	session = owner;

	/* The generation's model from its family record. */
	return session->node->gpu->identity.chip.profile->sm;
}

/*
 * Gives the 3D class the executor's push buffers bind.
 */
uint32_t
nvrtx_vk_threed_class(
	void *owner)
{
	struct nvrtx_session *session;

	session = owner;

	/* The generation's class from its family record. */
	return session->node->gpu->identity.chip.profile->class_threed;
}

/* Opens a session: it names its node and has an executor session of its own. */
static int
node_open(
	void *private_data,
	void **session)
{
	struct nvrtx_session *opened;
	int error;

	/* Allocates the session, with the lock of its executor. */
	opened = kern_calloc(1U, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	(void)mutex_init(&opened->exec_lock, LOCK_RANK_DEVICE, "nvrtx session executor");

	/* Opens its executor session. */
	error = nvrtx_vk_session_open(opened, &opened->vk);
	if (error != 0) {
		kern_free(opened);
		return error;
	}

	/* Succeeded: the session is the caller's. */
	opened->node = private_data;
	*session = opened;
	return 0;
}

/* Closes a session: its lease ends (the console surface comes back), its executor session and its channel close, then it goes. */
static void
node_close(
	void *private_data,
	void *session)
{
	struct nvrtx_node *node;
	struct nvrtx_session *closing;
	int error;

	node = private_data;
	closing = session;

	/* Ends the session's lease, if it holds it. */
	nvrtx_present_session_close(node, session);

	/* Closes the executor session holding it; a payload still held keeps it (and is shown). */
	mutex_lock(&closing->exec_lock);
	mutex_lock(&node->lock);

	error = nvrtx_vk_session_close(&closing->vk);

	mutex_unlock(&node->lock);
	mutex_unlock(&closing->exec_lock);

	if (error != 0)
		nvrtx_stage_mark("gpu-device: an executor session closed with %d", error);

	/* Frees the session's channel, its runs being over. */
	nvrtx_runtime_session_close(node, closing);

	/* Frees the session. */
	kern_free(session);
}

/* Describes the device to userspace. */
static int
node_get_info(
	void *private_data,
	void *session,
	struct gpu_info *info)
{
	struct nvrtx_node *node;

	UNUSED_PARAMETER(session);

	node = private_data;

	/* The capabilities (the renderer's too when it is bound) and the limits of the resources. */
	info->capabilities = node->ops.capabilities;
	info->max_resources = NODE_MAX_RESOURCES;
	info->max_resource_bytes = NODE_MAX_RESOURCE_BYTES;

	/* Names the driver, with the terminating NUL. */
	kern_memset(info->driver_name, 0, sizeof(info->driver_name));
	kern_memcpy(info->driver_name, "nvrtx", 6U);

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
	struct nvrtx_resource *resource;
	uint64_t size;
	int error;

	UNUSED_PARAMETER(private_data);
	UNUSED_PARAMETER(session);

	/* The core bounded the size; the run is whole pages. */
	if (request->bytes == 0U || request->bytes > NODE_MAX_RESOURCE_BYTES)
		return EINVAL;
	size = (request->bytes + NODE_PAGE_BYTES - 1U) & ~((uint64_t)NODE_PAGE_BYTES - 1U);

	/* Allocates the record. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* Allocates the RAM. */
	error = kern_pmem_alloc((size_t)size, NODE_PAGE_BYTES, &resource->ram);
	if (error != 0) {
		kern_free(resource);
		return ENOMEM;
	}

	/* Clears it through the direct map. */
	resource->cpu = kern_pmem_to_kernel(resource->ram.paddr);
	if (resource->cpu == NULL) {
		(void)kern_pmem_free(&resource->ram);
		kern_free(resource);
		return EIO;
	}

	kern_memset(resource->cpu, 0, (size_t)size);
	resource->kind = NVRTX_RESOURCE_STORAGE;
	resource->bytes = request->bytes;

	/* Succeeded: the storage is the session's. */
	*object = resource;
	return 0;
}

/*
 * Destroys a resource of either kind: a blob on the screen is taken off it
 * first; one that could not be is kept (its VRAM is never reused).
 */
static void
node_resource_destroy(
	void *private_data,
	void *session,
	void *object)
{
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;

	UNUSED_PARAMETER(session);

	node = private_data;
	resource = object;

	/* Storage (a blob only the CPU touches leaves its session's list first): its RAM goes back. */
	if (resource->kind == NVRTX_RESOURCE_STORAGE) {
		node_blob_unlisted(node, resource);
		(void)kern_pmem_free(&resource->ram);
		kern_free(resource);
		return;
	}

	/* A blob leaves its session's list and the screen first. */
	node_blob_unlisted(node, resource);
	nvrtx_present_resource_gone(node, resource);

	/* Drops its reference to the VRAM; the last one gives it back. */
	nvrtx_node_backing_put(node, resource->backing);

	/* Frees the record. */
	kern_free(resource);
}

/*
 * Creates a blob: a buffer object in the VRAM, mapped in the CPU's window.
 *
 * The sharing flags need nothing more: every blob is VRAM the other opens
 * of the device can import (share.c); another device cannot (no physical
 * view is lent).
 */
static int
node_blob_create(
	void *private_data,
	void *session,
	const struct gpu_blob_create *request,
	void **object,
	uint32_t *resource_id)
{
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;
	struct nvrtx_backing *backing;
	int error;

	node = private_data;

	/* The core bounded the size. */
	if (request->bytes == 0U || request->bytes > NODE_MAX_RESOURCE_BYTES)
		return EINVAL;

	/*
	 * A blob only the CPU touches, not a VkDeviceMemory's storage, is cached
	 * RAM: the executor reads a stream there through the CPU's cache, not
	 * word by word through the uncached window.
	 */
	if ((request->flags & GPU_BLOB_HOST) != 0U && request->blob_id == 0U) {
		error = node_host_blob(node, session, request->bytes, object, resource_id);
		if (error != 0)
			return error;

		/* Succeeded: the blob is the session's, in RAM. */
		return 0;
	}

	/* Allocates the record. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* Allocates the backing's record. */
	backing = kern_calloc(1U, sizeof(*backing));
	if (backing == NULL) {
		kern_free(resource);
		return ENOMEM;
	}

	/*
	 * Takes the VRAM and clears it, so no earlier contents of the VRAM show.
	 * It goes into the CPU's window only when the CPU first reaches it
	 * (nvrtx_node_backing_cpu): the window is far smaller than the VRAM, and
	 * most blobs (images the GPU draws and the display shows) never need it.
	 */
	error = nvrtx_bo_alloc(node->gpu, request->bytes, NODE_BLOB_ALIGN, &backing->bo);
	if (error == 0)
		error = nvrtx_bo_clear(node->gpu, &backing->bo);

	/*
	 * VRAM the copy engine may still write is never given back: the blob
	 * takes other VRAM, which the CPU clears (the copy engine's clears
	 * are off from then on).
	 */
	if (error == EBUSY) {
		error = nvrtx_bo_alloc(node->gpu, request->bytes, NODE_BLOB_ALIGN, &backing->bo);
		if (error == 0)
			error = nvrtx_bo_clear(node->gpu, &backing->bo);
	}
	if (error != 0) {
		/* What the copy engine may still write stays taken. */
		if (error != EBUSY)
			nvrtx_bo_free(node->gpu, &backing->bo);
		kern_free(backing);
		kern_free(resource);
		return error;
	}

	/* The blob holds the backing's one reference. */
	backing->bytes = request->bytes;
	backing->refs = 1U;
	resource->kind = NVRTX_RESOURCE_BLOB;
	resource->bytes = request->bytes;
	resource->backing = backing;

	/*
	 * A blob of the renderer's made for a VkDeviceMemory (its blob_id is the
	 * memory's identity) is that memory's storage: the memory takes a
	 * reference to the backing and the work channel maps it.
	 */
	if (request->blob_id != 0U && node->renderer != 0) {
		error = nvrtx_vk_node_blob_attach(node, session, request->blob_id, backing, request->bytes);
		if (error != 0) {
			nvrtx_bo_free(node->gpu, &backing->bo);
			kern_free(backing);
			kern_free(resource);
			return error;
		}
	}

	/* Succeeded: the blob is the session's, numbered for the protocol and listed. */
	*resource_id = nvrtx_node_resource_id(node);
	nvrtx_node_blob_listed(node, session, resource, *resource_id);
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
	struct nvrtx_node *node,
	struct nvrtx_session *session,
	uint64_t bytes,
	void **object,
	uint32_t *resource_id)
{
	struct nvrtx_resource *resource;
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
	resource->kind = NVRTX_RESOURCE_STORAGE;
	resource->bytes = bytes;

	/* Succeeded: the blob is the session's, numbered for the protocol and listed. */
	*resource_id = nvrtx_node_resource_id(node);
	nvrtx_node_blob_listed(node, session, resource, *resource_id);
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
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;
	volatile uint8_t *source;
	uint8_t *target;
	uint32_t at;
	int error;

	UNUSED_PARAMETER(session);

	node = private_data;
	resource = object;
	target = buffer;

	/* The range lies inside the resource. */
	error = node_range(resource, offset, bytes);
	if (error != 0)
		return error;

	/* Storage is plain RAM. */
	if (resource->kind == NVRTX_RESOURCE_STORAGE) {
		kern_memcpy(target, resource->cpu + offset, bytes);
		return 0;
	}

	/* A blob is read byte by byte through the uncached window. */
	error = nvrtx_node_backing_cpu(node, resource->backing, &source);
	if (error != 0)
		return error;
	source += offset;
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
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;
	volatile uint8_t *target;
	const uint8_t *source;
	uint32_t at;
	int error;

	UNUSED_PARAMETER(session);

	node = private_data;
	resource = object;
	source = buffer;

	/* The range lies inside the resource. */
	error = node_range(resource, offset, bytes);
	if (error != 0)
		return error;

	/* Storage is plain RAM. */
	if (resource->kind == NVRTX_RESOURCE_STORAGE) {
		kern_memcpy(resource->cpu + offset, source, bytes);
		return 0;
	}

	/* A blob is written byte by byte through the uncached window, then read back once so the writes have landed. */
	error = nvrtx_node_backing_cpu(node, resource->backing, &target);
	if (error != 0)
		return error;
	resource->backing->cpu_written = 1;
	target += offset;
	for (at = 0U; at < bytes; at++)
		target[at] = source[at];
	if (bytes != 0U)
		(void)target[bytes - 1U];

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
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;
	volatile uint8_t *cpu;
	bool write_combine;
	int error;

	UNUSED_PARAMETER(session);

	node = private_data;
	resource = object;

	/* Storage: the run of RAM. */
	if (resource->kind == NVRTX_RESOURCE_STORAGE) {
		mapping->physical = resource->ram.paddr;
		mapping->address = resource->cpu;
		mapping->bytes = resource->ram.size;
		mapping->attributes = 0U;
		return 0;
	}

	/* A blob: its place in the window, which it takes now if it has none; the application may write it from now on. */
	error = nvrtx_node_backing_cpu(node, resource->backing, &cpu);
	if (error != 0)
		return error;
	resource->backing->cpu_written = 1;
	mapping->physical = nvrtx_bo_bus(node->gpu, &resource->backing->bo);
	mapping->address = (void *)(uintptr_t)cpu;
	mapping->bytes = resource->backing->bo.size;
	mapping->attributes = DRV_GPU_MAPPING_DEVICE;

	/*
	 * The application's writes into the window combine where the kernel
	 * takes the policy (amd64; another port refuses it), as the amdrdna
	 * driver's do: the host-visible memory is written far more than read
	 * (a frame's instance data), and the submission's locked instructions
	 * on the writing processor drain its combining buffers before the GPU
	 * reads.
	 */
	write_combine = vm_device_write_combine_supported();
	if (write_combine)
		mapping->attributes |= DRV_GPU_MAPPING_WRITE_COMBINE;

	/* Succeeded: the view covers the whole object. */
	return 0;
}

/* Reports whether a range of bytes lies inside a resource: EINVAL otherwise. */
static int
node_range(
	const struct nvrtx_resource *resource,
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

/*
 * Frees a node that was never registered: its frames' and cursor's VRAM
 * and the node.  The caller then stops the GSP, which no longer shows the
 * frames and frees the cursor channel.
 */
static void
node_free(
	struct nvrtx_node *node)
{
	unsigned index;

	/* Gives each frame's VRAM back (a frame never taken is skipped). */
	for (index = 0U; index < 2U; index++)
		nvrtx_bo_free(node->gpu, &node->frame_objects[index]);

	/* Gives the cursor's slots back (skipped when never taken). */
	nvrtx_bo_free(node->gpu, &node->cursor_object);

	/* Frees the node. */
	kern_free(node);
}

/*
 * Takes the messages the GSP sends while the device is up, a turn at a
 * time, until the stop for the reboot asks it to end.  The worker that
 * made the device has ended; a turn holds the GSP's lock, which the
 * device's own calls (a session's channel, the recovery) take too, so one
 * thread reads the channel at a time.  Between turns the lock is free, and
 * the drain sleeps a tick when a thread waits for it (the lock is not
 * handed over).  The wait shows the channel errors it meets.
 *
 * About every second it also reads the head's frame counter, so the
 * refresh count keeps up even while nobody waits for a boundary (the
 * 16-bit counter wraps after about 18 minutes at 60 Hz).
 */
static void
node_drain(
	void *argument)
{
	struct nvrtx_node *node;
	uint64_t count;
	uint64_t time_ns;
	uint64_t now;
	uint64_t sampled;
	uint32_t wanted;
	size_t length;
	int error;

	node = argument;
	sampled = 0U;

	/* Takes messages turn after turn; a turn ends when its time is spent, and the stop ends the turns. */
	for (;;) {
		/* Holds the GSP's lock through one turn. */
		nvrtx_rpc_lock(node->gpu);

		error = nvrtx_rpc_wait(node->gpu, NODE_DRAIN_NO_FUNCTION, 0U, NODE_DRAIN_TURN_US, &length);

		nvrtx_rpc_unlock(node->gpu);

		/* A failed turn is followed by a pause. */
		if (error != 0 && error != ETIMEDOUT)
			kern_usleep_range(NODE_DRAIN_PAUSE_US, NODE_DRAIN_PAUSE_US);

		/* The stop for the reboot reads the GSP's replies itself from here on. */
		if (node->drain_stop != 0)
			break;

		/* A thread waiting for the lock takes it while the drain sleeps a tick. */
		wanted = __atomic_load_n(&node->gpu->rpc_wanted, __ATOMIC_ACQUIRE);
		if (wanted != 0U)
			kern_usleep_range(NODE_DRAIN_YIELD_US, NODE_DRAIN_YIELD_US * 2U);

		/* The refresh count and the counts about every second, not every turn. */
		now = nvrtx_now_us();
		if (now - sampled < NODE_DRAIN_SAMPLE_US)
			continue;
		sampled = now;

		/* Brings the refresh count up to date. */
		nvrtx_present_refresh_sample(node, &count, &time_ns);

		/* Writes the counts when they are asked for and their time has come. */
		node_perf_report(node);
	}

	/* drain_running cleared tells the stop that nothing else reads the channel. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	node->drain_running = 0;
}

/*
 * Writes the counts of the last period into the kernel log and starts the
 * next (nvrtx.vk.perf=1): the push buffers the work channel ran and their
 * average time, the CPU's copies, the renderer's frames with their average
 * wait for the node's lock and their average hold of it, the flips with
 * their average wait for the head, the GPU's own time for the runs, the
 * draws they held, the command streams read through the window and the
 * window's use.
 */
static void
node_perf_report(
	struct nvrtx_node *node)
{
	struct nvrtx_node_perf taken;
	uint64_t now;
	uint64_t period;
	uint64_t used;
	uint64_t peak;
	uint64_t size;

	/* Nothing is written without nvrtx.vk.perf=1, nor before the period is over. */
	if (node->perf == 0)
		return;

	now = nvrtx_now_us();
	period = now - node->perf_since_us;
	if (period < NODE_PERF_PERIOD_US)
		return;

	/* Takes each count and clears it; adds that come meanwhile go to the next period. */
	taken.runs = __atomic_exchange_n(&node->counts.runs, 0U, __ATOMIC_RELAXED);
	taken.ended = __atomic_exchange_n(&node->counts.ended, 0U, __ATOMIC_RELAXED);
	taken.run_us = __atomic_exchange_n(&node->counts.run_us, 0U, __ATOMIC_RELAXED);
	taken.blocked = __atomic_exchange_n(&node->counts.blocked, 0U, __ATOMIC_RELAXED);
	taken.blocked_us = __atomic_exchange_n(&node->counts.blocked_us, 0U, __ATOMIC_RELAXED);
	taken.parked = __atomic_exchange_n(&node->counts.parked, 0U, __ATOMIC_RELAXED);
	taken.copies = __atomic_exchange_n(&node->counts.copies, 0U, __ATOMIC_RELAXED);
	taken.copy_bytes = __atomic_exchange_n(&node->counts.copy_bytes, 0U, __ATOMIC_RELAXED);
	taken.frames = __atomic_exchange_n(&node->counts.frames, 0U, __ATOMIC_RELAXED);
	taken.wait_us = __atomic_exchange_n(&node->counts.wait_us, 0U, __ATOMIC_RELAXED);
	taken.held_us = __atomic_exchange_n(&node->counts.held_us, 0U, __ATOMIC_RELAXED);
	taken.flips = __atomic_exchange_n(&node->counts.flips, 0U, __ATOMIC_RELAXED);
	taken.flip_us = __atomic_exchange_n(&node->counts.flip_us, 0U, __ATOMIC_RELAXED);
	taken.run_max_us = __atomic_exchange_n(&node->counts.run_max_us, 0U, __ATOMIC_RELAXED);
	taken.timed = __atomic_exchange_n(&node->counts.timed, 0U, __ATOMIC_RELAXED);
	taken.gpu_us = __atomic_exchange_n(&node->counts.gpu_us, 0U, __ATOMIC_RELAXED);
	taken.gpu_max_us = __atomic_exchange_n(&node->counts.gpu_max_us, 0U, __ATOMIC_RELAXED);
	taken.draws = __atomic_exchange_n(&node->counts.draws, 0U, __ATOMIC_RELAXED);
	taken.quads = __atomic_exchange_n(&node->counts.quads, 0U, __ATOMIC_RELAXED);
	taken.push_bytes = __atomic_exchange_n(&node->counts.push_bytes, 0U, __ATOMIC_RELAXED);
	taken.streams = __atomic_exchange_n(&node->counts.streams, 0U, __ATOMIC_RELAXED);
	taken.stream_bytes = __atomic_exchange_n(&node->counts.stream_bytes, 0U, __ATOMIC_RELAXED);
	taken.stream_us = __atomic_exchange_n(&node->counts.stream_us, 0U, __ATOMIC_RELAXED);
	taken.cursor_sets = __atomic_exchange_n(&node->counts.cursor_sets, 0U, __ATOMIC_RELAXED);
	taken.cursor_same = __atomic_exchange_n(&node->counts.cursor_same, 0U, __ATOMIC_RELAXED);
	taken.cursor_set_us = __atomic_exchange_n(&node->counts.cursor_set_us, 0U, __ATOMIC_RELAXED);
	taken.cursor_moves = __atomic_exchange_n(&node->counts.cursor_moves, 0U, __ATOMIC_RELAXED);
	taken.cursor_move_us = __atomic_exchange_n(&node->counts.cursor_move_us, 0U, __ATOMIC_RELAXED);
	node->perf_since_us = now;

	/* The runs and the frames, each line within the log's 79 columns: the runs a wait saw end, from their submission. */
	nvrtx_stage_mark("perf %llums: runs %llu, ended %llu avg %lluus max %lluus",
	    (unsigned long long)(period / 1000U),
	    (unsigned long long)taken.runs,
	    (unsigned long long)taken.ended,
	    (unsigned long long)node_perf_average(taken.run_us, taken.ended),
	    (unsigned long long)taken.run_max_us);
	nvrtx_stage_mark("perf: frames %llu wait %lluus held %lluus, flips %llu avg %lluus",
	    (unsigned long long)taken.frames,
	    (unsigned long long)node_perf_average(taken.wait_us, taken.frames),
	    (unsigned long long)node_perf_average(taken.held_us, taken.frames),
	    (unsigned long long)taken.flips,
	    (unsigned long long)node_perf_average(taken.flip_us, taken.flips));

	/* The GPU's own time for the timed runs, the executor's waits for its runs, and the parked markers. */
	nvrtx_stage_mark("perf: gpu %llu avg %lluus max %lluus, blocked %llu %lluus, parked %llu",
	    (unsigned long long)taken.timed,
	    (unsigned long long)node_perf_average(taken.gpu_us, taken.timed),
	    (unsigned long long)taken.gpu_max_us,
	    (unsigned long long)taken.blocked,
	    (unsigned long long)taken.blocked_us,
	    (unsigned long long)taken.parked);

	/* What the runs held, and the copies the CPU did. */
	nvrtx_stage_mark("perf: draws %llu quads %llu push %lluKiB, cpu copies %llu %lluKiB",
	    (unsigned long long)taken.draws,
	    (unsigned long long)taken.quads,
	    (unsigned long long)(taken.push_bytes / 1024U),
	    (unsigned long long)taken.copies,
	    (unsigned long long)(taken.copy_bytes / 1024U));

	/* The command streams read through the window, and the time it took. */
	nvrtx_stage_mark("perf: streams %llu %lluKiB %lluus",
	    (unsigned long long)taken.streams,
	    (unsigned long long)(taken.stream_bytes / 1024U),
	    (unsigned long long)taken.stream_us);

	/* The cursor's images and moves, each with the average time it held the node's lock. */
	nvrtx_stage_mark("perf: cursor sets %llu same %llu avg %lluus, moves %llu avg %lluus",
	    (unsigned long long)taken.cursor_sets,
	    (unsigned long long)taken.cursor_same,
	    (unsigned long long)node_perf_average(taken.cursor_set_us, taken.cursor_sets),
	    (unsigned long long)taken.cursor_moves,
	    (unsigned long long)node_perf_average(taken.cursor_move_us, taken.cursor_moves));

	/* The CPU's window: what is mapped now, the most at once, and its size. */
	nvrtx_bo_window_use(node->gpu, &used, &peak, &size);
	nvrtx_stage_mark("perf: window %lluKiB peak %lluKiB of %lluKiB",
	    (unsigned long long)(used >> 10),
	    (unsigned long long)(peak >> 10),
	    (unsigned long long)(size >> 10));
}

/* Reports a total's average over its count, zero for no count. */
static uint64_t
node_perf_average(
	uint64_t total,
	uint64_t count)
{
	/* Nothing was counted. */
	if (count == 0U)
		return 0U;

	/* Succeeded: the average. */
	return total / count;
}

/* Takes a blob out of its session's list (a blob never listed is left alone). */
static void
node_blob_unlisted(
	struct nvrtx_node *node,
	struct nvrtx_resource *resource)
{
	struct nvrtx_resource **link;

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

	resource->session = NULL;
	resource->next_blob = NULL;

	mutex_unlock(&node->lock);
}
