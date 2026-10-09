/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The native V3D node owns independently shareable RAM and verified GPU mappings. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-allocation.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/share.h"

/* These initial capabilities exclude Vulkan until the executor and compiler are bound. */
#define RENDER_CAPABILITIES (GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE | GPU_CAP_ALLOCATION_SHARE)
#define RENDER_RESOURCE_COUNT 128U
#define RENDER_RESOURCE_BYTES (256ULL * 1024U * 1024U)

/* The node, operation tables and native VA owner persist for the kernel lifetime. */
static struct bcm2711_render_device render_controller;

static int render_open(void *opaque, void **result);
static void render_close(void *opaque, void *private_session);
static int render_info(void *opaque, void *private_session, struct gpu_info *info);
static int render_create(void *opaque, void *private_session, const struct gpu_resource_create *request, void **result);
static int render_blob(void *opaque, void *private_session, const struct gpu_blob_create *request, void **result, uint32_t *identifier);
static int render_placed(void *opaque, void *private_session, const struct gpu_blob_create_placed *request, void **result, uint32_t *identifier);
static void render_destroy(void *opaque, void *private_session, void *private_resource);
static int render_read(void *opaque, void *private_session, void *private_resource, uint64_t offset, void *data, uint32_t bytes);
static int render_write(void *opaque, void *private_session, void *private_resource, uint64_t offset, const void *data, uint32_t bytes);
static int render_map(void *opaque, void *private_session, void *private_resource, struct drv_gpu_mapping *mapping);
static int render_export(void *opaque, void *private_session, void *private_resource, const struct gpu_image_descriptor *image, void **result);
static void render_release(void *opaque, void *shared);
static int render_import(void *opaque, void *private_session, void *shared, void **result, uint32_t *identifier);
static int render_backing(void *opaque, void *shared, struct drv_gpu_scanout_backing *backing);
static int render_device(void *opaque, void *private_session, struct gpu_device_info *info);
static int render_stop_begin(void *opaque, void *private_session, int error);
static int render_stop_poll(void *opaque, void *private_session);
static void render_fault(void *opaque, int error);
static int render_reset(void *opaque);
static int allocate_resource(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, const struct gpu_blob_create *request, const struct gpu_placement *placement, bool blob, void **result, uint32_t *identifier);
static int import_buffer(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, struct bcm2711_buffer *buffer, bool blob, bool mappable, bool shareable, void **result, uint32_t *identifier);
static bool render_ready(struct bcm2711_render_device *controller);
static bool session_ready(struct bcm2711_render_session *session);
static void bind_render(struct bcm2711_render_device *controller);

/*
 * Publishes the native renderer only after V1 through V10 safely retire boot work.
 */
int
bcm2711_render_register(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_render_device *controller;
	bool ready;
	int error;

	/* No userspace owner may inherit failed diagnostic storage or an uncertain MMU. */
	controller = &render_controller;
	if (controller->registered || controller->space.native != NULL)
		return EBUSY;
	controller->space.native = engine;
	ready = render_ready(controller);
	if (!ready || engine->hardware.diagnostic != NULL) {
		controller->space.native = NULL;
		return ENODEV;
	}

	/* Initializes one lifetime owner before the common node can admit opens. */
	error = mutex_init(&controller->mutex, LOCK_RANK_DEVICE, "bcm2711-render-owner");
	if (error != 0) {
		controller->space.native = NULL;
		return error;
	}

	/* Storage and sharing are complete; Vulkan capabilities remain a later binding. */
	bind_render(controller);
	error = drv_gpu_register(&controller->operations, controller, &controller->gpu);
	if (error != 0) {
		controller->space.native = NULL;
		return error;
	}

	/* Borrowed tables and the native translation owner persist after publication. */
	controller->registered = true;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "render node ready allocation/share; Vulkan pending");

	/* Succeeded: userspace can allocate, map and share checked native V3D storage. */
	return 0;
}

/*
 * Finds one live protocol resource while its caller holds the controller mutex.
 */
struct bcm2711_render_resource *
bcm2711_render_find(
	struct bcm2711_render_session *session,
	uint32_t identifier)
{
	struct bcm2711_render_resource *resource;

	/* Only this retained session's immutable local identities can resolve an allocation. */
	for (resource = session->resources; resource != NULL; resource = resource->next) {
		if (resource->identifier == identifier)
			return resource;
	}

	/* Succeeded: an absent or retired identity resolves to no resource. */
	return NULL;
}

/*
 * Arms uncertain allocation teardown before notifying the common GPU error domain.
 * The caller releases the controller mutex before publishing the error.
 */
void
bcm2711_render_fail(
	struct bcm2711_render_device *controller,
	int error)
{
	/* A failure cannot be observed by closing sessions until native teardown retains backing. */
	render_fault(controller, error);
	drv_gpu_report_error(controller->gpu, error);
}

/* Opens one independent renderer namespace without granting native command access. */
static int
render_open(
	void *opaque,
	void **result)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	bool ready;

	/* A faulted renderer must pass common checked recovery before admitting another open. */
	*result = NULL;
	controller = opaque;
	mutex_lock(&controller->mutex);

	ready = render_ready(controller);
	if (!ready || controller->sessions == UINT32_MAX) {
		mutex_unlock(&controller->mutex);
		return EIO;
	}

	/* One open retains its local identity counter and complete resource list. */
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL) {
		mutex_unlock(&controller->mutex);
		return ENOMEM;
	}

	/* Sessions count only complete opens, and zero permits checked global recovery. */
	session->device = controller;
	controller->sessions++;
	*result = session;

	mutex_unlock(&controller->mutex);

	/* Succeeded: common final close owns this independent renderer session. */
	return 0;
}

/* Consumes a session descriptor after common pins and every resource have retired. */
static void
render_close(
	void *opaque,
	void *private_session)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;

	/* Quarantined views retain storage independently of the closing session descriptor. */
	controller = opaque;
	session = private_session;
	mutex_lock(&controller->mutex);

	if (session->device != controller || session->resources != NULL ||
	    session->count != 0 || controller->sessions == 0)
		__builtin_trap();
	controller->sessions--;

	mutex_unlock(&controller->mutex);

	/* No allocation, job or capability retains a pointer to this retired open. */
	kern_free(session);
}

/* Reports only actual native storage operations implemented by this node. */
static int
render_info(
	void *opaque,
	void *private_session,
	struct gpu_info *info)
{
	struct bcm2711_render_device *controller;

	/* The immutable operation table supplies the authoritative negotiated capability mask. */
	(void)private_session;
	controller = opaque;
	info->capabilities = controller->operations.capabilities;
	info->max_resources = RENDER_RESOURCE_COUNT;
	info->max_resource_bytes = RENDER_RESOURCE_BYTES;
	kern_snprintf(info->driver_name, sizeof(info->driver_name), "bcm2711-v3d42");

	/* Succeeded: no Vulkan or command feature is implied by storage discovery. */
	return 0;
}

/* Creates ordinary native storage without blob or export semantics. */
static int
render_create(
	void *opaque,
	void *private_session,
	const struct gpu_resource_create *request,
	void **result)
{
	struct gpu_blob_create allocation;
	uint32_t identifier;
	int error;

	/* Only ordinary storage with no extra wire flags has a defined allocation contract. */
	*result = NULL;
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE || request->flags != 0)
		return ENOTSUP;
	kern_memset(&allocation, 0, sizeof(allocation));
	allocation.bytes = request->bytes;
	allocation.flags = GPU_BLOB_MAPPABLE;
	error = allocate_resource(opaque, private_session, &allocation, NULL, false, result, &identifier);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns ordinary CPU storage and a checked native VA view. */
	return 0;
}

/* Creates a checked native blob with selected mapping and sharing authority. */
static int
render_blob(
	void *opaque,
	void *private_session,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *identifier)
{
	int error;

	/* Allocation and VA publication preserve independent storage ownership on every failure. */
	error = allocate_resource(opaque, private_session, request, NULL, true, result, identifier);
	if (error != 0)
		return error;

	/* Succeeded: one complete native blob owns its allocation and protocol identity. */
	return 0;
}

/* Creates a blob whose actual physical run satisfies every accepted condition. */
static int
render_placed(
	void *opaque,
	void *private_session,
	const struct gpu_blob_create_placed *request,
	void **result,
	uint32_t *identifier)
{
	int error;

	/* Requested placement reaches the allocator before any native VA becomes visible. */
	error = allocate_resource(opaque, private_session, &request->blob, &request->placement, true, result, identifier);
	if (error != 0)
		return error;

	/* Succeeded: the native blob meets actual placement and translation requirements. */
	return 0;
}

/* Retires the descriptor while failed translation retirement retains its independent view. */
static void
render_destroy(
	void *opaque,
	void *private_session,
	void *private_resource)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	struct bcm2711_render_resource *resource;
	struct bcm2711_render_resource **position;
	int error;

	/* Common resource pins exclude concurrent access to this exact descriptor. */
	controller = opaque;
	session = private_session;
	resource = private_resource;
	mutex_lock(&controller->mutex);

	if (resource->owner != session || session->count == 0)
		__builtin_trap();

	/* Withdraws this resource identity before allowing its physical mapping to retire. */
	position = &session->resources;
	while (*position != NULL && *position != resource)
		position = &(*position)->next;

	/* A descriptor outside this session cannot consume another resource's reference. */
	if (*position == NULL)
		__builtin_trap();
	*position = resource->next;
	session->count--;
	error = bcm2711_v3d_memory_release(&controller->space, resource->view);

	mutex_unlock(&controller->mutex);

	/* Failure retains the independent view while ordinary descriptor teardown finishes. */
	kern_free(resource);
	if (error != 0)
		bcm2711_render_fail(controller, error);
}

/* Reads one complete CPU span while the controller excludes native execution. */
static int
render_read(
	void *opaque,
	void *private_session,
	void *private_resource,
	uint64_t offset,
	void *data,
	uint32_t bytes)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_resource *resource;
	struct bcm2711_buffer *buffer;

	/* Immutable allocation bounds cover every copied byte. */
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session)
		return EINVAL;
	buffer = resource->view->buffer;
	if (offset > buffer->bytes || bytes > buffer->bytes - offset)
		return EINVAL;
	mutex_lock(&controller->mutex);

	kern_memcpy(data, (const uint8_t *)buffer->address + offset, bytes);

	mutex_unlock(&controller->mutex);

	/* Succeeded: no temporary CPU destination remains borrowed by the renderer. */
	return 0;
}

/* Writes one complete CPU span before a later native job publishes its cache range. */
static int
render_write(
	void *opaque,
	void *private_session,
	void *private_resource,
	uint64_t offset,
	const void *data,
	uint32_t bytes)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_resource *resource;
	struct bcm2711_buffer *buffer;

	/* Shared CPU mutation remains bounded to this retained resource's actual bytes. */
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session)
		return EINVAL;
	buffer = resource->view->buffer;
	if (offset > buffer->bytes || bytes > buffer->bytes - offset)
		return EINVAL;
	mutex_lock(&controller->mutex);

	kern_memcpy((uint8_t *)buffer->address + offset, data, bytes);

	mutex_unlock(&controller->mutex);

	/* Succeeded: the native worker will clean input ranges at submission. */
	return 0;
}

/* Returns a stable CPU view retained by common VM pins, without exposing GPU page entries. */
static int
render_map(
	void *opaque,
	void *private_session,
	void *private_resource,
	struct drv_gpu_mapping *mapping)
{
	struct bcm2711_render_resource *resource;
	struct bcm2711_buffer *buffer;

	/* Mapping authority belongs to this exact open and its immutable blob flags. */
	(void)opaque;
	resource = private_resource;
	if (resource->owner != private_session || !resource->mappable)
		return ENOTSUP;
	buffer = resource->view->buffer;
	mapping->physical = buffer->memory.paddr;
	mapping->address = buffer->address;
	mapping->bytes = buffer->bytes;
	mapping->attributes = 0;

	/* Succeeded: the common core pins ordinary cached RAM through every VM mapping. */
	return 0;
}

/* Exports independent native storage without retaining its session or VA view. */
static int
render_export(
	void *opaque,
	void *private_session,
	void *private_resource,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_resource *resource;
	struct bcm2711_shared *shared;
	int error;

	/* Only a shareable blob can create an independently owned allocation capability. */
	*result = NULL;
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session || !resource->blob || !resource->shareable)
		return ENOTSUP;
	mutex_lock(&controller->mutex);

	error = bcm2711_shared_create(resource->view->buffer, image, &shared);

	mutex_unlock(&controller->mutex);

	/* A rejected image export retains no new capability ownership. */
	if (error != 0)
		return error;
	*result = shared;

	/* Succeeded: source-session teardown cannot invalidate the exported native allocation. */
	return 0;
}

/* Consumes one exported capability after independent imports have taken their own holds. */
static void
render_release(
	void *opaque,
	void *shared)
{
	/* The common capability keeps its page vector alive until this exact release. */
	(void)opaque;
	bcm2711_shared_release(shared);
}

/* Imports native storage into a new session-specific VA and local resource identity. */
static int
render_import(
	void *opaque,
	void *private_session,
	void *shared,
	void **result,
	uint32_t *identifier)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_buffer *buffer;
	bool ready;
	int error;

	/* The retained common capability guarantees the borrowed allocation survives this call. */
	controller = opaque;
	buffer = bcm2711_shared_buffer(shared);
	mutex_lock(&controller->mutex);

	error = import_buffer(controller, private_session, buffer, true, true, true, result, identifier);
	ready = render_ready(controller);

	mutex_unlock(&controller->mutex);

	/* Failed translation publication keeps uncertain RAM in the native view owner. */
	if (error != 0) {
		if (!ready)
			bcm2711_render_fail(controller, error);
		return error;
	}

	/* Succeeded: this open owns its independent native view and resource identity. */
	return 0;
}

/* Supplies verified actual physical pages for a retained native image capability. */
static int
render_backing(
	void *opaque,
	void *shared,
	struct drv_gpu_scanout_backing *backing)
{
	int error;

	/* Allocation-only exports cannot become foreign scanout without an authoritative layout. */
	(void)opaque;
	error = bcm2711_shared_backing(shared, backing);
	if (error != 0)
		return error;

	/* Succeeded: the display can validate the real placement and acquire its own native hold. */
	return 0;
}

/* Identifies the independent renderer without inventing a core-owned companion ID. */
static int
render_device(
	void *opaque,
	void *private_session,
	struct gpu_device_info *info)
{
	/* Companion zero expresses no preference; the display's foreign constraints select sharing. */
	(void)opaque;
	(void)private_session;
	info->roles = GPU_DEVICE_RENDER;
	info->companion_id = 0;
	info->flags = 0;

	/* Succeeded: the common core stamps the exact identity of this independent render node. */
	return 0;
}

/* Closes this namespace's native publication without waiting for hardware. */
static int
render_stop_begin(
	void *opaque,
	void *private_session,
	int error)
{
	struct bcm2711_render_session *session;
	unsigned long enabled;
	struct bcm2711_render_device *controller;

	/* The initial allocation-only node publishes no native jobs or retained completions. */
	(void)error;
	controller = opaque;
	session = private_session;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	session->stopping = true;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: no later request may publish native work for this namespace. */
	return 0;
}

/* Confirms the allocation-only namespace has no published job or callback. */
static int
render_stop_poll(
	void *opaque,
	void *private_session)
{
	struct bcm2711_render_device *controller;
	struct bcm2711_render_session *session;
	unsigned long enabled;
	int error;

	/* A native queue becoming busy invalidates this allocation-only stopping contract. */
	controller = opaque;
	session = private_session;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	error = 0;
	if (!session->stopping || controller->space.native->hardware.job_busy)
		error = EAGAIN;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* A pending stop supplies no DMA retirement evidence. */
	if (error != 0)
		return error;

	/* Succeeded: this allocation-only session has no native job or posted callback. */
	return 0;
}

/* Arms all resource destruction to preserve native DMA storage before common loss publication. */
static void
render_fault(
	void *opaque,
	int error)
{
	struct bcm2711_render_device *controller;
	unsigned long enabled;

	/* Sticky native fault closes admission without acquiring the potentially occupied worker mutex. */
	(void)error;
	controller = opaque;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	controller->space.native->hardware.faulted = true;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
}

/* Reclaims uncertain translations only after common owners and checked native reset retire. */
static int
render_reset(
	void *opaque)
{
	struct bcm2711_render_device *controller;
	int error;

	/* The common framework serializes fresh recovery after every old external owner closes. */
	controller = opaque;
	mutex_lock(&controller->mutex);

	if (controller->sessions != 0) {
		mutex_unlock(&controller->mutex);
		return EBUSY;
	}

	/* Native reset verifies provider, all identifiers, MMU/cache and serviced IRQ admission. */
	error = bcm2711_v3d_hardware_reset(controller->space.native);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Native translation flush must retire every quarantined VA before physical reuse. */
	error = bcm2711_v3d_memory_recover(&controller->space);

	mutex_unlock(&controller->mutex);

	/* Failed recovery retains every remaining native allocation reservation. */
	if (error != 0)
		return error;

	/* Succeeded: a new namespace can allocate and publish native translations again. */
	return 0;
}

/* Allocates source storage and publishes its native view under one controller transaction. */
static int
allocate_resource(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	const struct gpu_blob_create *request,
	const struct gpu_placement *placement,
	bool blob,
	void **result,
	uint32_t *identifier)
{
	struct bcm2711_buffer *buffer;
	bool mappable;
	bool shareable;
	bool ready;
	int error;

	/* A complete allocation never exceeds the public resource extent or count. */
	*result = NULL;
	*identifier = 0;
	if (request->bytes > RENDER_RESOURCE_BYTES)
		return ENOTSUP;
	mutex_lock(&controller->mutex);

	ready = session_ready(session);
	if (!ready) {
		mutex_unlock(&controller->mutex);
		return EIO;
	}

	/* Acquires one source reference before creating the session's independent native view. */
	error = bcm2711_blob_allocate(request, placement, &buffer);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* The request chooses CPU mapping and export authority independently. */
	mappable = false;
	shareable = false;
	if ((request->flags & GPU_BLOB_MAPPABLE) != 0)
		mappable = true;
	if ((request->flags & GPU_BLOB_SHAREABLE) != 0)
		shareable = true;
	error = import_buffer(controller, session, buffer, blob, mappable, shareable, result, identifier);
	bcm2711_buffer_release(buffer);
	ready = render_ready(controller);

	mutex_unlock(&controller->mutex);

	/* Translation uncertainty is quarantined before other sessions observe common device loss. */
	if (error != 0) {
		if (!ready)
			bcm2711_render_fail(controller, error);
		return error;
	}

	/* Succeeded: the common resource owns its complete local descriptor and native VA view. */
	return 0;
}

/* Creates one resource descriptor whose native view owns an independent allocation reference. */
static int
import_buffer(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	struct bcm2711_buffer *buffer,
	bool blob,
	bool mappable,
	bool shareable,
	void **result,
	uint32_t *identifier)
{
	struct bcm2711_render_resource *resource;
	bool ready;
	int error;

	/* A closing, faulted or exhausted namespace cannot acquire another identity. */
	*result = NULL;
	*identifier = 0;
	ready = session_ready(session);
	if (!ready)
		return EIO;
	if (session->count >= RENDER_RESOURCE_COUNT || session->next_identifier == UINT32_MAX)
		return ENOSPC;
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* A failed hardware flush retains its view independently of this unpublished descriptor. */
	error = bcm2711_v3d_memory_map(&controller->space, buffer, &resource->view);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* Publishes one complete resource after both translation caches have acknowledged it. */
	resource->owner = session;
	resource->blob = blob;
	resource->mappable = mappable;
	resource->shareable = shareable;
	session->next_identifier++;
	resource->identifier = session->next_identifier;
	resource->next = session->resources;
	session->resources = resource;
	session->count++;
	*identifier = resource->identifier;
	*result = resource;

	/* Succeeded: this retained session owns exactly one mapped resource descriptor. */
	return 0;
}

/* Samples native admission under the IRQ owner's persistent fault guard. */
static bool
render_ready(
	struct bcm2711_render_device *controller)
{
	unsigned long enabled;
	bool ready;

	/* An asynchronous MMU/cache/job failure closes every namespace before new mappings publish. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	ready = false;
	if (controller->space.native->hardware.ready &&
	    !controller->space.native->hardware.faulted)
		ready = true;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: the caller sees the current native allocation admission state. */
	return ready;
}

/* Binds complete allocation, mapping, sharing and checked recovery operations before publication. */
static void
bind_render(
	struct bcm2711_render_device *controller)
{
	/* The allocation-only tables never imply SPIR-V compilation or Vulkan execution. */
	controller->operations.version = DRV_GPU_INTERFACE_VERSION;
	controller->operations.size = sizeof(controller->operations);
	controller->operations.capabilities = RENDER_CAPABILITIES;
	controller->operations.open = render_open;
	controller->operations.close = render_close;
	controller->operations.get_info = render_info;
	controller->operations.resource_create = render_create;
	controller->operations.resource_destroy = render_destroy;
	controller->operations.blob_create = render_blob;
	controller->operations.blob_create_placed = render_placed;
	controller->operations.resource_read = render_read;
	controller->operations.resource_write = render_write;
	controller->operations.resource_map = render_map;
	controller->operations.share = &controller->share_operations;
	controller->operations.scanout = &controller->scanout_operations;
	controller->operations.recovery = &controller->recovery_operations;

	/* Native allocation capabilities retain physical storage outside the exporting open. */
	controller->share_operations.export_resource = render_export;
	controller->share_operations.release = render_release;
	controller->share_operations.import_resource = render_import;
	controller->share_operations.get_scanout_backing = render_backing;
	controller->scanout_operations.query_device = render_device;

	/* Checked global reset follows all external owner retirement, never ordinary destruction. */
	controller->recovery_operations.stop_begin = render_stop_begin;
	controller->recovery_operations.stop_poll = render_stop_poll;
	controller->recovery_operations.fault = render_fault;
	controller->recovery_operations.reset = render_reset;
}

/* Samples this namespace's stop admission under the same guard as native fault publication. */
static bool
session_ready(
	struct bcm2711_render_session *session)
{
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;
	bool ready;

	/* A concurrent common stop closes publication even while another ioctl holds the mutex. */
	hardware = &session->device->space.native->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	ready = false;
	if (hardware->ready && !hardware->faulted && !session->stopping)
		ready = true;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Succeeded: the caller sees both global native admission and this namespace's stop. */
	return ready;
}
