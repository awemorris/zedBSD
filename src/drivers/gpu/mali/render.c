/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali render node (the second GPU node, after the display's): its
 * sessions, their address spaces, their resources and blobs, the CPU's
 * view of them, and their export to other sessions and to the display.
 *
 * A buffer object is one run of memory below the GPU's reach with an
 * uncached view (the GPU does not snoop the processor's caches); a
 * resource maps it at an address of one session's space.  The command
 * frames, the jobs and the recovery are sched.c's.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-scanout.h>

#include "drivers/gpu/mali/render.h"

/* How often the registration is tried while the core is busy, and the pause between tries. */
#define REGISTER_ATTEMPTS	50U
#define REGISTER_PAUSE_MS	100U

/* The ring of the node's stream: 64 KiB of instructions. */
#define RING_BYTES		0x10000ULL

static int render_open(void *device, void **result);
static void render_close(void *device, void *session);
static int render_info(void *device, void *session, struct gpu_info *info);
static int render_resource_create(void *device, void *session, const struct gpu_resource_create *request, void **result);
static void render_resource_destroy(void *device, void *session, void *resource);
static int render_blob_create(void *device, void *session, const struct gpu_blob_create *request, void **result, uint32_t *identifier);
static int render_read(void *device, void *session, void *resource, uint64_t offset, void *bytes, uint32_t length);
static int render_write(void *device, void *session, void *resource, uint64_t offset, const void *bytes, uint32_t length);
static int render_map(void *device, void *session, void *resource, struct drv_gpu_mapping *mapping);
static int render_export(void *device, void *session, void *resource, const struct gpu_image_descriptor *image, void **result);
static void render_release(void *device, void *shared);
static int render_import(void *device, void *session, void *shared, uint32_t flags, void **result, uint32_t *identifier);
static int render_backing(void *device, void *shared, struct drv_gpu_scanout_backing *backing);
static int render_query(void *device, void *session, struct gpu_device_info *info);
static int space_take(struct mali_space_runs *runs, uint64_t bytes, uint64_t *address);
static void space_give(struct mali_space_runs *runs, uint64_t address, uint64_t bytes);
static int make_pages(struct mali_bo *bo);
static void session_free(struct mali_render *render, struct mali_session *session);
static int map_fixed(struct mali_render *render, struct mali_address_space *space);

/*
 * The board's one render node.  Filled by mali_render_start() on the
 * later stage's thread before the node is published; its fields are then
 * guarded as struct mali_render says.
 */
static struct mali_render board_render;

/* The node's sharing operations: exports to other sessions and the display's view of them. */
static const struct drv_gpu_share_ops share_operations = {
	.export_resource = render_export,
	.release = render_release,
	.import_resource = render_import,
	.get_scanout_backing = render_backing,
};

/* The node's role: rendering, with no display of its own. */
static const struct drv_gpu_scanout_ops scanout_operations = {
	.query_device = render_query,
};

/*
 * Publishes the render node over the running firmware: the node's ring
 * and sync page, the idle tables of its slot, its stream, then the
 * registration.
 */
int
mali_render_start(
	struct mali_device *device)
{
	struct mali_render *render;
	unsigned tries;
	int error;

	/* The node's state. */
	render = &board_render;
	kern_memset(render, 0, sizeof(*render));
	render->device = device;
	spin_init(&render->guard, LOCK_RANK_DEVICE, "mali-render");

	/* The ring and the sync page every session's space maps. */
	error = mali_bo_create(render, RING_BYTES, &render->ring);
	if (error == 0)
		error = mali_bo_create(render, MALI_PAGE_BYTES, &render->sync);
	if (error != 0) {
		kern_logf("mali: render: ring not made (%d)\n", error);
		return error;
	}

	/* The idle tables of the node's slot: only the ring and the sync page. */
	error = mali_as_init(device, &render->idle, MALI_RENDER_ADDRESS_SPACE);
	if (error == 0)
		error = map_fixed(render, &render->idle);
	if (error == 0)
		error = mali_as_enable(device, &render->idle);
	if (error != 0) {
		kern_logf("mali: render: address space %u not made (%d)\n", MALI_RENDER_ADDRESS_SPACE, error);
		return error;
	}

	/* The stream, started on the empty ring. */
	error = mali_sched_start(render);
	if (error != 0)
		return error;

	/* The operation table. */
	render->ops.version = DRV_GPU_INTERFACE_VERSION;
	render->ops.size = sizeof(render->ops);
	render->ops.capabilities = GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE | GPU_CAP_NOTIFICATION;
	render->ops.open = render_open;
	render->ops.close = render_close;
	render->ops.get_info = render_info;
	render->ops.resource_create = render_resource_create;
	render->ops.resource_destroy = render_resource_destroy;
	render->ops.blob_create = render_blob_create;
	render->ops.resource_read = render_read;
	render->ops.resource_write = render_write;
	render->ops.resource_map = render_map;
	render->ops.share = &share_operations;
	render->ops.scanout = &scanout_operations;
	mali_sched_bind_ops(render);

	/* Registers the node, trying again while the core is busy. */
	error = EBUSY;
	for (tries = 0; tries < REGISTER_ATTEMPTS; tries++) {
		error = drv_gpu_register(&render->ops, render, &render->node);
		if (error != EBUSY)
			break;
		sched_sleep(sched_ticks() + kern_ms_to_ticks(REGISTER_PAUSE_MS));
	}

	if (error != 0) {
		kern_logf("mali: render node not registered (%d)\n", error);
		return error;
	}

	/* Succeeded: the node is published. */
	device->render = render;
	kern_logf("mali: render node ready (stream on group %u, address space %u, ring at %llx)\n",
		  render->queue.group_slot,
		  MALI_RENDER_ADDRESS_SPACE,
		  (unsigned long long)MALI_RENDER_RING_ADDRESS);
	return 0;
}

/*
 * Makes a buffer object of whole pages, zeroed, with one reference.
 */
int
mali_bo_create(
	struct mali_render *render,
	uint64_t bytes,
	struct mali_bo **result)
{
	struct mali_bo *bo;
	int error;

	/* A size the node takes. */
	(void)render;
	*result = NULL;
	if (bytes == 0U || bytes > MALI_RENDER_MAX_BYTES)
		return EINVAL;

	/* The bookkeeping. */
	bo = kern_calloc(1, sizeof(*bo));
	if (bo == NULL)
		return ENOMEM;

	/* The run and its uncached view. */
	error = mali_buffer_alloc(&bo->buffer, bytes);
	if (error != 0) {
		kern_free(bo);
		return error;
	}

	/* Succeeded: the caller holds the one reference. */
	bo->references = 1;
	*result = bo;
	return 0;
}

/* Takes one more reference on a buffer object. */
void
mali_bo_retain(
	struct mali_render *render,
	struct mali_bo *bo)
{
	unsigned long enabled;

	/* The count under the node's guard. */
	enabled = spin_lock_irqsave(&render->guard);

	bo->references++;

	spin_unlock_irqrestore(&render->guard, enabled);
}

/* Lets one reference go; the last frees the run. */
void
mali_bo_put(
	struct mali_render *render,
	struct mali_bo *bo)
{
	unsigned long enabled;
	uint32_t left;

	/* No object. */
	if (bo == NULL)
		return;

	/* The count under the node's guard. */
	enabled = spin_lock_irqsave(&render->guard);

	bo->references--;
	left = bo->references;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* The last holder frees the run, the page list and the bookkeeping. */
	if (left != 0U)
		return;
	mali_buffer_free(&bo->buffer);
	kern_free(bo->pages);
	kern_free(bo);
}

/*
 * Makes a session's handle on a buffer object, mapped at a free address
 * of the session's space; the handle takes a reference of its own.
 */
int
mali_resource_make(
	struct mali_session *session,
	struct mali_bo *bo,
	bool mappable,
	struct mali_resource **result)
{
	struct mali_render *render;
	struct mali_resource *resource;
	uint64_t address;
	int error;

	/* Room among the session's handles. */
	render = session->render;
	*result = NULL;
	if (session->resource_count >= MALI_RENDER_RESOURCES)
		return ENOSPC;

	/* The handle. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* A free run of the space. */
	error = space_take(&session->runs, bo->buffer.bytes, &address);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* The mapping, readable and writable, cached in the GPU's L2 (every buffer is shared uncached with the processor). */
	mutex_lock(&render->device->lock);

	error = mali_as_map(render->device, &session->space, address, &bo->buffer, MALI_PT_WRITE | MALI_PT_EXECUTE);

	mutex_unlock(&render->device->lock);

	if (error != 0) {
		(void)mali_pt_unmap(&session->space.tables, address, bo->buffer.bytes);
		space_give(&session->runs, address, bo->buffer.bytes);
		kern_free(resource);
		return error;
	}

	/* The handle holds the object and is listed by the session. */
	mali_bo_retain(render, bo);
	resource->session = session;
	resource->bo = bo;
	resource->gpu_address = address;
	resource->mappable = mappable;
	resource->next = session->resources;
	session->resources = resource;
	session->resource_count++;

	/* Succeeded: the GPU sees the object at the address in this session. */
	*result = resource;
	return 0;
}

/*
 * Unmaps and drops one handle.  Every submission has ended when this
 * runs (they run one at a time and wait), so the GPU no longer reads it.
 */
void
mali_resource_unmake(
	struct mali_session *session,
	struct mali_resource *resource)
{
	struct mali_render *render;
	struct mali_resource **position;

	/* Unlists it. */
	render = session->render;
	position = &session->resources;
	while (*position != NULL && *position != resource)
		position = &(*position)->next;
	if (*position != NULL) {
		*position = resource->next;
		session->resource_count--;
	}

	/* Unmaps its run; a session the GPU may still reach keeps its mapping and its object. */
	if (session->isolated)
		return;

	mutex_lock(&render->device->lock);

	(void)mali_as_unmap(render->device, &session->space, resource->gpu_address, resource->bo->buffer.bytes);

	mutex_unlock(&render->device->lock);

	/* The address and the object go back. */
	space_give(&session->runs, resource->gpu_address, resource->bo->buffer.bytes);
	mali_bo_put(render, resource->bo);
	kern_free(resource);
}

/* Finds a blob of the session by the number it was named by, or NULL. */
struct mali_resource *
mali_session_blob(
	struct mali_session *session,
	uint32_t identifier)
{
	struct mali_resource *resource;

	/* Zero names no blob. */
	if (identifier == 0U)
		return NULL;

	/* Walks the session's handles. */
	for (resource = session->resources; resource != NULL; resource = resource->next) {
		if (resource->identifier == identifier)
			return resource;
	}

	/* No such blob. */
	return NULL;
}

/*
 * Takes bytes of the session's stage memory for the submission being
 * built, aligned, with their uncached view and GPU address; a new chunk
 * is made when the last has no room.
 */
int
mali_stage_take(
	struct mali_session *session,
	uint64_t bytes,
	uint64_t align,
	volatile uint8_t **view,
	uint64_t *gpu_address)
{
	struct mali_resource *resource;
	struct mali_bo *bo;
	uint64_t chunk;
	uint64_t at;
	int error;

	/* A power-of-two alignment of at least eight. */
	if (bytes == 0U || align == 0U || (align & (align - 1U)) != 0U)
		return EINVAL;
	if (align < 8U)
		align = 8U;

	/* What the last chunk has left. */
	if (session->stage_count != 0U) {
		bo = session->stage[session->stage_count - 1U];
		at = (session->stage_used + align - 1U) & ~(align - 1U);
		if (at <= bo->buffer.bytes && bytes <= bo->buffer.bytes - at) {
			*view = bo->buffer.view + at;
			*gpu_address = session->stage_address[session->stage_count - 1U] + at;
			session->stage_used = at + bytes;
			return 0;
		}
	}

	/* A new chunk, at least as large as asked. */
	if (session->stage_count >= MALI_RENDER_STAGE_CHUNKS)
		return ENOSPC;
	chunk = MALI_RENDER_STAGE_BYTES;
	if (bytes > chunk)
		chunk = (bytes + MALI_PAGE_BYTES - 1U) & ~(MALI_PAGE_BYTES - 1U);
	error = mali_bo_create(session->render, chunk, &bo);
	if (error != 0)
		return error;

	/* Mapped in the session's space; the resource is only the mapping's record. */
	error = mali_resource_make(session, bo, false, &resource);
	mali_bo_put(session->render, bo);
	if (error != 0)
		return error;

	session->stage[session->stage_count] = bo;
	session->stage_address[session->stage_count] = resource->gpu_address;
	session->stage_count++;
	session->stage_used = 0U;

	/* The bytes from its start. */
	*view = bo->buffer.view;
	*gpu_address = resource->gpu_address;
	session->stage_used = bytes;

	/* Succeeded: the bytes are the submission's. */
	return 0;
}

/*
 * Empties the session's stage after a submission ended: the first chunk
 * is kept for the next one, the others are unmapped and freed.
 */
void
mali_stage_reset(
	struct mali_session *session)
{
	struct mali_resource *resource;
	struct mali_resource *next;
	uint32_t index;
	bool extra;

	/* The resources of every chunk past the first. */
	for (resource = session->resources; resource != NULL; resource = next) {
		next = resource->next;
		extra = false;
		for (index = 1; index < session->stage_count; index++) {
			if (resource->bo == session->stage[index])
				extra = true;
		}

		if (extra)
			mali_resource_unmake(session, resource);
	}

	/* Only the first chunk is left, empty. */
	if (session->stage_count > 1U)
		session->stage_count = 1U;
	session->stage_used = 0U;
}

/*
 * Finds the image export whose backing the display node was handed and
 * holds its object for the display: the page list render_backing() gave
 * out names the object.  ENOTSUP for a backing of another node, or when
 * the node was never registered.
 */
int
mali_render_scanout_hold(
	const struct drv_gpu_scanout_backing *backing,
	struct mali_bo **bo)
{
	struct mali_render *render;
	struct mali_shared *shared;
	unsigned long enabled;

	/* A node that was never registered handed out nothing (and its guard may not be made). */
	render = &board_render;
	*bo = NULL;
	if (render->node == NULL)
		return ENOTSUP;

	/*
	 * The listed image export whose page list is the backing's.  The
	 * reference is taken under the guard, so the export's release cannot
	 * free the object in between.
	 */
	enabled = spin_lock_irqsave(&render->guard);

	for (shared = render->exports; shared != NULL; shared = shared->next) {
		if (!shared->has_image)
			continue;
		if (shared->bo->pages == NULL || shared->bo->pages != backing->pages)
			continue;
		shared->bo->references++;
		*bo = shared->bo;
		break;
	}

	spin_unlock_irqrestore(&render->guard, enabled);

	/* Not one of this node's exports. */
	if (*bo == NULL)
		return ENOTSUP;

	/* Succeeded: the object lives until the display drops it. */
	return 0;
}

/* Drops the display node's hold on an exported object; the last holder frees it. */
void
mali_render_scanout_drop(
	struct mali_bo *bo)
{
	/* The node's count, under its guard. */
	mali_bo_put(&board_render, bo);
}

/* Opens a session: its space, with the ring and the sync page at their places, and its free runs. */
static int
render_open(
	void *device,
	void **result)
{
	struct mali_render *render;
	struct mali_session *session;
	unsigned long enabled;
	int error;

	/* Refuses a new session once the node faulted. */
	render = device;
	*result = NULL;
	if (render->faulted)
		return EIO;

	/* The bookkeeping, with the whole of the session's part free. */
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;
	session->render = render;
	session->runs.start[0] = MALI_RENDER_SPACE_START;
	session->runs.bytes[0] = MALI_RENDER_SPACE_END - MALI_RENDER_SPACE_START;
	session->runs.count = 1U;

	/* The space, bound to the node's slot only while its work runs. */
	error = mali_as_init(render->device, &session->space, MALI_RENDER_ADDRESS_SPACE);
	if (error == 0)
		error = map_fixed(render, &session->space);
	if (error != 0) {
		mali_as_destroy(&session->space);
		kern_free(session);
		return error;
	}

	/* Listed by the node. */
	enabled = spin_lock_irqsave(&render->guard);

	session->next = render->sessions;
	render->sessions = session;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* Succeeded: the session is the core's. */
	*result = session;
	return 0;
}

/*
 * Closes a session after the core destroyed its resources: the
 * executor's objects, the stage, the slot's binding, then the tables.
 */
static void
render_close(
	void *device,
	void *opaque)
{
	struct mali_render *render;
	struct mali_session *session;
	struct mali_session **position;
	unsigned long enabled;

	/* Unlists it. */
	render = device;
	session = opaque;
	enabled = spin_lock_irqsave(&render->guard);

	position = &render->sessions;
	while (*position != NULL && *position != session)
		position = &(*position)->next;
	if (*position != NULL)
		*position = session->next;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* The executor's objects and memory first. */
	mali_vk_close(session->vk);
	session->vk = NULL;

	/* The slot forgets the session's tables. */
	mali_sched_forget(render, session);

	/* A session the GPU may still reach keeps everything until a reset. */
	if (session->isolated)
		return;

	/* The rest. */
	session_free(render, session);
}

/* Reports the node's capabilities and limits. */
static int
render_info(
	void *device,
	void *session,
	struct gpu_info *info)
{
	struct mali_render *render;

	/* The node's table holds the capabilities. */
	render = device;
	(void)session;
	info->capabilities = render->ops.capabilities;
	info->max_resources = MALI_RENDER_RESOURCES;
	info->max_resource_bytes = MALI_RENDER_MAX_BYTES;
	kern_memcpy(info->driver_name, "mali", sizeof("mali"));

	/* Succeeded: the snapshot is filled. */
	return 0;
}

/* Makes one storage allocation of the session, mapped and host visible. */
static int
render_resource_create(
	void *device,
	void *opaque,
	const struct gpu_resource_create *request,
	void **result)
{
	struct mali_render *render;
	struct mali_resource *resource;
	struct mali_bo *bo;
	int error;

	/* Only storage, with no flags. */
	render = device;
	*result = NULL;
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE || request->flags != 0U)
		return EINVAL;

	/* The object. */
	error = mali_bo_create(render, request->bytes, &bo);
	if (error != 0)
		return error;

	/* The session's handle on it; the handle took a reference of its own. */
	error = mali_resource_make(opaque, bo, true, &resource);
	mali_bo_put(render, bo);
	if (error != 0)
		return error;

	/* Succeeded: the resource is the core's. */
	*result = resource;
	return 0;
}

/* Destroys one resource of the session. */
static void
render_resource_destroy(
	void *device,
	void *opaque,
	void *object)
{
	/* Unmaps it and drops its object. */
	(void)device;
	mali_resource_unmake(opaque, object);
}

/*
 * Makes one blob of the session.  A blob of a VkDeviceMemory (its blob
 * id is the memory's identity) maps the memory's own object; any other
 * has an object of its own.
 */
static int
render_blob_create(
	void *device,
	void *opaque,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *identifier)
{
	struct mali_render *render;
	struct mali_session *session;
	struct mali_resource *resource;
	struct mali_bo *bo;
	bool mappable;
	int error;

	/* The defined flags; a cross-device blob must be shareable. */
	render = device;
	session = opaque;
	*result = NULL;
	*identifier = 0;
	if ((request->flags & ~(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0U)
		return EINVAL;
	if ((request->flags & GPU_BLOB_CROSS_DEVICE) != 0U && (request->flags & GPU_BLOB_SHAREABLE) == 0U)
		return EINVAL;

	/* The object: the memory's, retained, or a new one. */
	if (request->blob_id != 0U) {
		if (session->vk == NULL)
			return EINVAL;
		error = mali_vk_memory_bo(session->vk, request->blob_id, request->bytes, &bo);
	} else {
		error = mali_bo_create(render, request->bytes, &bo);
	}

	if (error != 0)
		return error;

	/* The session's handle on it, mappable when asked. */
	mappable = false;
	if ((request->flags & GPU_BLOB_MAPPABLE) != 0U)
		mappable = true;
	error = mali_resource_make(session, bo, mappable, &resource);
	mali_bo_put(render, bo);
	if (error != 0)
		return error;

	/* Succeeded: the blob, named by the session's next number. */
	session->next_identifier++;
	if (session->next_identifier == 0U)
		session->next_identifier = 1U;
	resource->identifier = session->next_identifier;
	*identifier = resource->identifier;
	*result = resource;
	return 0;
}

/* Copies bytes out of a resource through its uncached view. */
static int
render_read(
	void *device,
	void *opaque,
	void *object,
	uint64_t offset,
	void *bytes,
	uint32_t length)
{
	struct mali_resource *resource;
	uint64_t size;

	/* Refuses a range outside the object. */
	(void)device;
	(void)opaque;
	resource = object;
	size = resource->bo->buffer.bytes;
	if (offset > size || length > size - offset)
		return EINVAL;

	/* The copy. */
	kern_memcpy(bytes, (const void *)(uintptr_t)(resource->bo->buffer.view + offset), length);

	/* Succeeded: the bytes are copied. */
	return 0;
}

/* Copies bytes into a resource through its uncached view. */
static int
render_write(
	void *device,
	void *opaque,
	void *object,
	uint64_t offset,
	const void *bytes,
	uint32_t length)
{
	struct mali_resource *resource;
	uint64_t size;

	/* Refuses a range outside the object. */
	(void)device;
	(void)opaque;
	resource = object;
	size = resource->bo->buffer.bytes;
	if (offset > size || length > size - offset)
		return EINVAL;

	/* The copy; the bytes are in memory when it returns. */
	kern_memcpy((void *)(uintptr_t)(resource->bo->buffer.view + offset), bytes, length);

	/* Succeeded: the bytes are copied. */
	return 0;
}

/* Describes the CPU view of a mappable resource: its whole pages, normal non-cacheable RAM. */
static int
render_map(
	void *device,
	void *opaque,
	void *object,
	struct drv_gpu_mapping *mapping)
{
	struct mali_resource *resource;

	/* Only a mappable resource of this session has a view. */
	(void)device;
	resource = object;
	if (resource->session != opaque || !resource->mappable)
		return ENOTSUP;

	/* The run, which the user mapping keeps uncached as the kernel's view is. */
	mapping->physical = resource->bo->buffer.run.paddr;
	mapping->address = (void *)(uintptr_t)resource->bo->buffer.view;
	mapping->bytes = resource->bo->buffer.bytes;
	mapping->attributes = DRV_GPU_MAPPING_UNCACHED_RAM;

	/* Succeeded: the core maps the run. */
	return 0;
}

/* Exports a resource's object, as an image or as an allocation only. */
static int
render_export(
	void *device,
	void *opaque,
	void *object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct mali_render *render;
	struct mali_resource *resource;
	struct mali_shared *shared;
	unsigned long enabled;
	int error;

	/* Only a resource of this session. */
	render = device;
	resource = object;
	*result = NULL;
	if (resource->session != opaque)
		return EINVAL;

	/* An image lies inside the object and needs its page list. */
	if (image != NULL) {
		if (image->offset > resource->bo->buffer.bytes ||
		    (uint64_t)image->stride * image->height > resource->bo->buffer.bytes - image->offset)
			return EINVAL;
		error = make_pages(resource->bo);
		if (error != 0)
			return error;
	}

	/* The export's bookkeeping. */
	shared = kern_calloc(1, sizeof(*shared));
	if (shared == NULL)
		return ENOMEM;
	shared->bo = resource->bo;
	if (image != NULL) {
		shared->image = *image;
		shared->has_image = true;
	}

	/* Holds the object, and lists the export. */
	mali_bo_retain(render, resource->bo);
	enabled = spin_lock_irqsave(&render->guard);

	shared->next = render->exports;
	render->exports = shared;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* Succeeded: the export lives independently of the session. */
	*result = shared;
	return 0;
}

/* Ends an export. */
static void
render_release(
	void *device,
	void *object)
{
	struct mali_render *render;
	struct mali_shared *shared;
	struct mali_shared **position;
	unsigned long enabled;

	/* Unlists the export. */
	render = device;
	shared = object;
	enabled = spin_lock_irqsave(&render->guard);

	position = &render->exports;
	while (*position != NULL && *position != shared)
		position = &(*position)->next;
	if (*position != NULL)
		*position = shared->next;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* Drops the export's hold on the object. */
	mali_bo_put(render, shared->bo);
	kern_free(shared);
}

/* Imports an export of this node into a session as a mappable resource of its own. */
static int
render_import(
	void *device,
	void *opaque,
	void *object,
	uint32_t flags,
	void **result,
	uint32_t *identifier)
{
	struct mali_session *session;
	struct mali_shared *shared;
	struct mali_resource *resource;
	int error;

	/* The export the core kept alive for this call. */
	(void)device;
	(void)flags;
	session = opaque;
	shared = object;
	*result = NULL;
	*identifier = 0;

	/* A new handle, mapped into this session. */
	error = mali_resource_make(session, shared->bo, true, &resource);
	if (error != 0)
		return error;

	/* Succeeded: the resource is the session's, under its next number. */
	session->next_identifier++;
	if (session->next_identifier == 0U)
		session->next_identifier = 1U;
	resource->identifier = session->next_identifier;
	*identifier = resource->identifier;
	*result = resource;
	return 0;
}

/* Describes an image export's pages for a display: one contiguous run. */
static int
render_backing(
	void *device,
	void *object,
	struct drv_gpu_scanout_backing *backing)
{
	struct mali_shared *shared;

	/* Only an image export has a layout to show. */
	(void)device;
	shared = object;
	if (!shared->has_image || shared->bo->pages == NULL)
		return ENOTSUP;

	/* The run's pages, contiguous, in RAM. */
	backing->pages = shared->bo->pages;
	backing->page_count = shared->bo->page_count;
	backing->bytes = shared->bo->buffer.bytes;
	backing->page_bytes = (uint32_t)MALI_PAGE_BYTES;
	backing->flags = DRV_GPU_BACKING_CONTIGUOUS;

	/* Succeeded: the view stays valid while the export lives. */
	return 0;
}

/* Reports the node's role: rendering only. */
static int
render_query(
	void *device,
	void *session,
	struct gpu_device_info *info)
{
	/* The core fills the identity; the node names no companion. */
	(void)device;
	(void)session;
	info->roles = GPU_DEVICE_RENDER;
	info->companion_id = 0;

	/* Succeeded: the role is filled. */
	return 0;
}

/* Takes the first free run of a space that holds bytes at the objects' alignment. */
static int
space_take(
	struct mali_space_runs *runs,
	uint64_t bytes,
	uint64_t *address)
{
	uint64_t rounded;
	uint32_t index;

	/* Whole alignment steps, so every object starts aligned. */
	rounded = (bytes + MALI_RENDER_SPACE_ALIGN - 1U) & ~(MALI_RENDER_SPACE_ALIGN - 1U);

	/* The first run large enough gives up its start. */
	for (index = 0; index < runs->count; index++) {
		if (runs->bytes[index] < rounded)
			continue;
		*address = runs->start[index];
		runs->start[index] += rounded;
		runs->bytes[index] -= rounded;
		if (runs->bytes[index] == 0U) {
			kern_memmove(&runs->start[index], &runs->start[index + 1U], (runs->count - index - 1U) * sizeof(runs->start[0]));
			kern_memmove(&runs->bytes[index], &runs->bytes[index + 1U], (runs->count - index - 1U) * sizeof(runs->bytes[0]));
			runs->count--;
		}
		return 0;
	}

	/* No run holds it. */
	return ENOSPC;
}

/*
 * Gives a run back, merged with the free runs next to it; a run that
 * cannot be listed (the list is full) is lost to the space.
 */
static void
space_give(
	struct mali_space_runs *runs,
	uint64_t address,
	uint64_t bytes)
{
	uint64_t rounded;
	uint32_t index;

	/* The place in the sorted list. */
	rounded = (bytes + MALI_RENDER_SPACE_ALIGN - 1U) & ~(MALI_RENDER_SPACE_ALIGN - 1U);
	index = 0;
	while (index < runs->count && runs->start[index] < address)
		index++;

	/* Joins the run before it. */
	if (index > 0U && runs->start[index - 1U] + runs->bytes[index - 1U] == address) {
		runs->bytes[index - 1U] += rounded;
		if (index < runs->count && address + rounded == runs->start[index]) {
			runs->bytes[index - 1U] += runs->bytes[index];
			kern_memmove(&runs->start[index], &runs->start[index + 1U], (runs->count - index - 1U) * sizeof(runs->start[0]));
			kern_memmove(&runs->bytes[index], &runs->bytes[index + 1U], (runs->count - index - 1U) * sizeof(runs->bytes[0]));
			runs->count--;
		}
		return;
	}

	/* Joins the run after it. */
	if (index < runs->count && address + rounded == runs->start[index]) {
		runs->start[index] = address;
		runs->bytes[index] += rounded;
		return;
	}

	/* A run of its own, when the list has room. */
	if (runs->count >= MALI_RENDER_SPACE_RUNS)
		return;
	kern_memmove(&runs->start[index + 1U], &runs->start[index], (runs->count - index) * sizeof(runs->start[0]));
	kern_memmove(&runs->bytes[index + 1U], &runs->bytes[index], (runs->count - index) * sizeof(runs->bytes[0]));
	runs->start[index] = address;
	runs->bytes[index] = rounded;
	runs->count++;
}

/* Makes a buffer object's page list, once. */
static int
make_pages(
	struct mali_bo *bo)
{
	uint64_t count;
	uint64_t index;

	/* Made already. */
	if (bo->pages != NULL)
		return 0;

	/* One address per page of the run. */
	count = bo->buffer.bytes / MALI_PAGE_BYTES;
	bo->pages = kern_calloc((size_t)count, sizeof(bo->pages[0]));
	if (bo->pages == NULL)
		return ENOMEM;
	for (index = 0; index < count; index++)
		bo->pages[index] = bo->buffer.run.paddr + index * MALI_PAGE_BYTES;
	bo->page_count = count;

	/* Succeeded: the list lives as long as the object. */
	return 0;
}

/* Frees what a closed session still holds: its handles, its stage and its tables. */
static void
session_free(
	struct mali_render *render,
	struct mali_session *session)
{
	/* Every handle left (the stage's among them). */
	(void)render;
	while (session->resources != NULL)
		mali_resource_unmake(session, session->resources);

	/* The tables, which no slot translates through any more. */
	mali_as_destroy(&session->space);
	kern_free(session);
}

/* Maps the node's ring and sync page at their fixed places of a space. */
static int
map_fixed(
	struct mali_render *render,
	struct mali_address_space *space)
{
	int error;

	/* The ring, which the front end reads. */
	error = mali_pt_map(&space->tables, MALI_RENDER_RING_ADDRESS, render->ring->buffer.run.paddr, render->ring->buffer.bytes, MALI_PT_WRITE);
	if (error != 0)
		return error;

	/* The sync page, which the front end adds to. */
	error = mali_pt_map(&space->tables, MALI_RENDER_SYNC_ADDRESS, render->sync->buffer.run.paddr, render->sync->buffer.bytes, MALI_PT_WRITE);
	if (error != 0)
		return error;

	/* Succeeded: both are at their places. */
	return 0;
}
