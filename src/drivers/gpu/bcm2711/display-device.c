/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native display leases copy into device-owned RAM that outlives every session. */
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"

/* Resource limits describe actual ordinary packed-pixel storage support. */
#define DISPLAY_RESOURCE_BYTES (32ULL * 1024U * 1024U)
#define DISPLAY_RESOURCE_COUNT 64U
#define DISPLAY_CAPABILITIES (GPU_CAP_RESOURCE | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS)

/* One open owns its resources and at most one nontransferrable display lease. */
struct display_session {
	uint64_t lease;
	uint64_t completed;
	uint64_t present_time;
	uint32_t resources;
};

/* One ordinary storage allocation belongs to one open until common VM pins retire. */
struct display_resource {
	struct display_session *owner;
	struct bcm2711_buffer *buffer;
};

/* One boot output owns persistent operation tables and private scanout buffers. */
struct display_device {
	struct mutex mutex;
	struct bcm2711_display *native;
	struct drv_gpu_device *gpu;
	struct drv_gpu_ops operations;
	struct drv_gpu_display_ops display_operations;
	struct drv_gpu_scanout_ops scanout_operations;
	struct bcm2711_buffer *copies[2];
	struct display_session *owner;
	uint64_t next_lease;
	uint64_t completed;
	uint64_t present_time;
	bool registered;
};

/* The node and its borrowed tables persist for the entire kernel lifetime. */
static struct display_device display_controller;

static int device_open(void *opaque, void **result);
static void device_close(void *opaque, void *private_session);
static int device_info(void *opaque, void *private_session, struct gpu_info *info);
static int resource_create(void *opaque, void *private_session, const struct gpu_resource_create *request, void **result);
static void resource_destroy(void *opaque, void *private_session, void *private_resource);
static int resource_read(void *opaque, void *private_session, void *private_resource, uint64_t offset, void *data, uint32_t bytes);
static int resource_write(void *opaque, void *private_session, void *private_resource, uint64_t offset, const void *data, uint32_t bytes);
static int resource_map(void *opaque, void *private_session, void *private_resource, struct drv_gpu_mapping *mapping);
static int output_query(void *opaque, void *private_session, struct gpu_display_info *request);
static int output_mode(void *opaque, void *private_session, struct gpu_display_mode *request);
static int output_claim(void *opaque, void *private_session, struct gpu_display_claim *request);
static int output_release(void *opaque, void *private_session, const struct gpu_display_release *request);
static int output_present(void *opaque, void *private_session, void *private_resource, struct gpu_display_present *request);
static int output_wait(void *opaque, void *private_session, struct gpu_display_wait *request);
static int output_events(void *opaque, void *private_session, uint64_t *sequence);
static int output_device(void *opaque, void *private_session, struct gpu_device_info *info);
static int output_constraints(void *opaque, void *private_session, struct gpu_scanout_constraints *request);
static int find_output(const struct display_device *controller, uint32_t identifier, uint64_t generation);
static int restore_console(struct display_device *controller);
static int copy_frame(struct display_device *controller, const struct display_resource *resource, const struct gpu_display_present *request, struct drv_bcm2711_boot_screen *frame);
static int observe_frames(struct bcm2711_display *display);
static int exercise_display(struct display_device *controller);
static void bind_operations(struct display_device *controller);

/*
 * Registers a confirmed boot output with owned copy storage and complete leases.
 * Registration failures unwind only after the console has no borrowed DMA holds.
 */
int
bcm2711_display_register(
	struct bcm2711_display *display)
{
	struct display_device *controller;
	bool allowed;
	uint32_t slot;
	int error;
	int detached;

	/* Keeps diagnostic stop points effective before allocating or publishing a node. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "P1");
	if (!allowed)
		return ECANCELED;
	if (display->screen.size > DISPLAY_RESOURCE_BYTES)
		return ENOTSUP;
	if (!display->scanout_started || display->refresh_millihz == 0)
		return ENODEV;

	/* Reports one bounded cadence observation before allocating or changing the plane. */
	error = observe_frames(display);
	if (error != 0)
		return error;
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "P2");
	if (!allowed)
		return ECANCELED;

	/* Creates one persistent controller; repeated registration never resets owners. */
	controller = &display_controller;
	if (controller->registered || controller->native != NULL)
		return EBUSY;
	error = mutex_init(&controller->mutex, LOCK_RANK_DEVICE, "bcm2711-display-owner");
	if (error != 0)
		return error;
	controller->native = display;
	controller->next_lease = 1;
	controller->completed = 0;

	/* Preallocates both copy targets before userspace can claim this output. */
	for (slot = 0; slot < 2; slot++) {
		/* Each native target is contiguous and entirely below the one-GiB HVS limit. */
		error = bcm2711_buffer_create(display->screen.size, 0x3fffffffU, 4096, &controller->copies[slot]);
		if (error != 0)
			break;
	}

	/* No scanout operation exists yet, so allocation failure has no DMA uncertainty. */
	if (error != 0) {
		while (slot != 0) {
			/* Releases each successful allocation before abandoning this attempt. */
			slot--;
			bcm2711_buffer_release(controller->copies[slot]);
			controller->copies[slot] = NULL;
		}

		/* No native publication occurred, so another boot attempt owns no state. */
		controller->native = NULL;
		return error;
	}

	/* Attaches the already checked boot pipeline before publishing callbacks. */
	error = bcm2711_display_flip_attach(display);
	if (error != 0) {
		bcm2711_buffer_release(controller->copies[0]);
		bcm2711_buffer_release(controller->copies[1]);
		controller->copies[0] = NULL;
		controller->copies[1] = NULL;
		controller->native = NULL;
		return error;
	}

	/* Exercises actual native flip and composition before offering the output to userspace. */
	error = exercise_display(controller);
	if (error == 0) {
		/* A P5 stop preserves the observed P1/P2/P3 result without registering a node. */
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "P5");
		if (!allowed)
			error = ECANCELED;
	}

	/* Publishes only the operations implemented by this display-only device. */
	if (error == 0) {
		bind_operations(controller);
		error = drv_gpu_register(&controller->operations, controller, &controller->gpu);
	}

	/* Unwinds only when the native console owns every active and pending reference. */
	if (error != 0) {
		detached = bcm2711_display_flip_detach(display);
		if (detached != 0)
			return error;
		bcm2711_buffer_release(controller->copies[0]);
		bcm2711_buffer_release(controller->copies[1]);
		controller->copies[0] = NULL;
		controller->copies[1] = NULL;
		controller->native = NULL;
		return error;
	}

	/* Borrowed state and copy buffers now persist independently of open descriptions. */
	controller->registered = true;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P5 display node ready hdmi%u copy FIFO", display->port);

	/* Succeeded: userspace can enumerate and lease the actual boot output. */
	return 0;
}

/* Counts selected-PV frames over one bounded second without holding the IRQ guard. */
static int
observe_frames(
	struct bcm2711_display *display)
{
	struct bcm2711_flip_status before;
	struct bcm2711_flip_status after;
	uint32_t step;

	/* Leaves the native console visible while measuring actual serviced frame sources. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P1 begin vblank observation");
	bcm2711_display_flip_snapshot(display, &before);
	for (step = 0; step < 1000U; step++) {
		/* Millisecond waits allow the selected PV IRQ to update its persistent counter. */
		kern_usleep_range(1000, 1000);
	}

	/* The captured cadence is evidence for the physical test, never an assumed 60 Hz. */
	bcm2711_display_flip_snapshot(display, &after);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P1 frames %llu in 1s expected %u mHz",
			   (unsigned long long)(after.frame_sequence - before.frame_sequence),
			   display->refresh_millihz);
	if (after.frame_sequence == before.frame_sequence)
		return ETIMEDOUT;

	/* Succeeded: at least one actual selected-PV frame arrived during observation. */
	return 0;
}

/* Presents both owned targets and one translucent patch, retiring all DMA before return. */
static int
exercise_display(
	struct display_device *controller)
{
	struct bcm2711_display *display;
	struct drv_bcm2711_boot_screen frame;
	uint8_t *console;
	uint8_t *pixel;
	uint32_t slot;
	uint32_t row;
	uint32_t column;
	bool allowed;
	int error;
	int restored;

	/* The diagnostics use the same CPU views and physical runs as subsequent copy presents. */
	display = controller->native;
	console = kern_pmem_to_kernel(display->screen.physical);
	if (console == NULL)
		return EFAULT;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P2 begin two owned buffers");
	bcm2711_stage_pause(BCM2711_FAMILY_DISPLAY, "P2");

	/* Two distinct top stripes make consecutive frame adoption visible in a photograph. */
	error = 0;
	for (slot = 0; slot < 2U; slot++) {
		/* A fresh console copy keeps lower text intact under an opaque primary. */
		kern_memcpy(controller->copies[slot]->address, console, (size_t)display->screen.size);
		for (row = 0; row < display->screen.height / 8U; row++) {
			/* Uses green then purple, independent of the native red/blue byte order. */
			pixel = (uint8_t *)controller->copies[slot]->address + (uint64_t)row * display->screen.pitch;
			for (column = 0; column < display->screen.width; column++) {
				/* The opaque stripe leaves the rest of the retained text view unchanged. */
				pixel[column * 4U] = (uint8_t)(slot * 0xc0U);
				pixel[column * 4U + 1U] = (uint8_t)((1U - slot) * 0xc0U);
				pixel[column * 4U + 2U] = (uint8_t)(slot * 0xc0U);
				pixel[column * 4U + 3U] = 0xff;
			}
		}

		/* A submitted target stays immutable until a later confirmed list retires it. */
		frame = display->screen;
		frame.physical = controller->copies[slot]->memory.paddr;
		error = bcm2711_display_flip_present(display, &frame);
		if (error != 0)
			break;
		bcm2711_stage_pause(BCM2711_FAMILY_DISPLAY, "P2 adopted");
	}

	/* Console adoption, even after timeout, is the only permission to reuse both targets. */
	restored = bcm2711_display_flip_restore(display);
	if (restored != 0)
		return restored;
	if (error != 0)
		return error;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P2 two flips and console adopted");
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "P3");
	if (!allowed)
		return ECANCELED;

	/* Places one compact premultiplied checker at the upper right without scaling. */
	frame = display->screen;
	if (frame.width > 256U)
		frame.width = 256;
	if (frame.height > 128U)
		frame.height = 128;
	frame.pitch = frame.width * 4U;
	frame.size = (uint64_t)frame.pitch * frame.height;
	frame.physical = controller->copies[0]->memory.paddr;
	for (row = 0; row < frame.height; row++) {
		/* All colors are premultiplied for half-opacity HVS pixel alpha. */
		pixel = (uint8_t *)controller->copies[0]->address + (uint64_t)row * frame.pitch;
		for (column = 0; column < frame.width; column++) {
			/* Alternating white and green expose both blending and upper-plane positioning. */
			pixel[column * 4U] = (uint8_t)(((column / 16U + row / 16U) & 1U) * 0x80U);
			pixel[column * 4U + 1U] = 0x80;
			pixel[column * 4U + 2U] = pixel[column * 4U];
			pixel[column * 4U + 3U] = 0x80;
		}
	}

	/* Actual two-plane composition uses a fresh console list and fresh upper contexts. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P3 begin premultiplied checker");
	bcm2711_stage_pause(BCM2711_FAMILY_DISPLAY, "P3");
	error = bcm2711_display_flip_compose(display, &frame, display->screen.width - frame.width, 0, true);
	if (error == 0)
		bcm2711_stage_pause(BCM2711_FAMILY_DISPLAY, "P3 adopted");

	/* Any failed retirement preserves allocations in the permanent unpublished controller. */
	restored = bcm2711_display_flip_restore(display);
	if (restored != 0)
		return restored;
	if (error != 0)
		return error;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P3 composed frame and console adopted");

	/* Succeeded: no diagnostic buffer remains in the active or pending HVS list. */
	return 0;
}

/* Creates a session without changing scanout or acquiring the output lease. */
static int
device_open(
	void *opaque,
	void **result)
{
	struct display_session *session;

	/* Each open starts with no resource or output ownership. */
	(void)opaque;
	*result = NULL;
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;
	*result = session;

	/* Succeeded: the common GPU core owns this independent session. */
	return 0;
}

/* Closes a lease owner while retaining controller buffers across failed restoration. */
static void
device_close(
	void *opaque,
	void *private_session)
{
	struct display_device *controller;
	struct display_session *session;
	int error;

	/* Stops referring to the closing session even if DMA retirement cannot be proved. */
	controller = opaque;
	session = private_session;
	mutex_lock(&controller->mutex);

	if (controller->owner == session) {
		/* Copy targets belong to the permanent controller, never to this session. */
		error = restore_console(controller);
		if (error != 0)
			bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "lease close retains uncertain copy buffers (%d)", error);
		controller->owner = NULL;
		session->lease = 0;
	}

	mutex_unlock(&controller->mutex);

	/* The common core retired every resource/VM pin before invoking final close. */
	kern_free(session);
}

/* Reports storage and display capabilities without promising Vulkan execution. */
static int
device_info(
	void *opaque,
	void *private_session,
	struct gpu_info *info)
{
	/* The controller exposes only independently implemented callbacks. */
	(void)opaque;
	(void)private_session;
	info->capabilities = DISPLAY_CAPABILITIES;
	info->max_resources = DISPLAY_RESOURCE_COUNT;
	info->max_resource_bytes = DISPLAY_RESOURCE_BYTES;
	kern_snprintf(info->driver_name, sizeof(info->driver_name), "bcm2711-display");

	/* Succeeded: resource limits describe actual CPU-addressable storage. */
	return 0;
}

/* Creates one session-owned storage resource with contiguous immutable placement. */
static int
resource_create(
	void *opaque,
	void *private_session,
	const struct gpu_resource_create *request,
	void **result)
{
	struct display_device *controller;
	struct display_session *session;
	struct display_resource *resource;
	int error;

	/* Validates storage semantics before reserving session allocation capacity. */
	*result = NULL;
	if (request->bytes == 0 || request->bytes > DISPLAY_RESOURCE_BYTES)
		return EINVAL;
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE || request->flags != 0)
		return ENOTSUP;
	controller = opaque;
	session = private_session;
	mutex_lock(&controller->mutex);

	if (session->resources >= DISPLAY_RESOURCE_COUNT) {
		mutex_unlock(&controller->mutex);
		return ENOSPC;
	}

	/* Acquires the resource descriptor before its physical storage. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL) {
		mutex_unlock(&controller->mutex);
		return ENOMEM;
	}

	/* Keeps ordinary storage directly usable for later native-device sharing. */
	error = bcm2711_buffer_create(request->bytes, 0x3fffffffU, 4096, &resource->buffer);
	if (error != 0) {
		kern_free(resource);
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Publishes ownership only after the allocation and CPU view are complete. */
	resource->owner = session;
	session->resources++;
	*result = resource;

	mutex_unlock(&controller->mutex);

	/* Succeeded: common resource teardown owns the descriptor and its initial reference. */
	return 0;
}

/* Retires ordinary storage after pins; no HVS DMA ever borrows these bytes. */
static void
resource_destroy(
	void *opaque,
	void *private_session,
	void *private_resource)
{
	struct display_device *controller;
	struct display_session *session;
	struct display_resource *resource;

	/* Drops capacity independently of the persistent private scanout targets. */
	controller = opaque;
	session = private_session;
	resource = private_resource;
	mutex_lock(&controller->mutex);

	if (resource->owner != session || session->resources == 0)
		__builtin_trap();
	session->resources--;
	bcm2711_buffer_release(resource->buffer);

	mutex_unlock(&controller->mutex);

	/* Only common resource pins kept this descriptor alive. */
	kern_free(resource);
}

/* Copies bytes from one retained resource under its controller ownership mutex. */
static int
resource_read(
	void *opaque,
	void *private_session,
	void *private_resource,
	uint64_t offset,
	void *data,
	uint32_t bytes)
{
	struct display_device *controller;
	struct display_resource *resource;

	/* Bounds the entire CPU copy before acquiring storage for transfer. */
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session)
		return EINVAL;
	if (offset > resource->buffer->bytes || bytes > resource->buffer->bytes - offset)
		return EINVAL;
	mutex_lock(&controller->mutex);

	kern_memcpy(data, (const uint8_t *)resource->buffer->address + offset, bytes);

	mutex_unlock(&controller->mutex);

	/* Succeeded: ordinary transfer finished before returning to the common core. */
	return 0;
}

/* Copies bytes into a resource that is never used directly for this display's DMA. */
static int
resource_write(
	void *opaque,
	void *private_session,
	void *private_resource,
	uint64_t offset,
	const void *data,
	uint32_t bytes)
{
	struct display_device *controller;
	struct display_resource *resource;

	/* Uses immutable allocation bounds to reject a truncated transfer. */
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session)
		return EINVAL;
	if (offset > resource->buffer->bytes || bytes > resource->buffer->bytes - offset)
		return EINVAL;
	mutex_lock(&controller->mutex);

	kern_memcpy((uint8_t *)resource->buffer->address + offset, data, bytes);

	mutex_unlock(&controller->mutex);

	/* Succeeded: no future device read depends on the caller's temporary CPU buffer. */
	return 0;
}

/* Returns a stable ordinary-RAM view retained by common VM pins. */
static int
resource_map(
	void *opaque,
	void *private_session,
	void *private_resource,
	struct drv_gpu_mapping *mapping)
{
	struct display_resource *resource;

	/* The physical placement and CPU address never change during this resource's life. */
	(void)opaque;
	resource = private_resource;
	if (resource->owner != private_session)
		return EINVAL;
	mapping->physical = resource->buffer->memory.paddr;
	mapping->address = resource->buffer->address;
	mapping->bytes = resource->buffer->bytes;
	mapping->attributes = 0;

	/* Succeeded: the mapping describes cached RAM rather than MMIO. */
	return 0;
}

/* Enumerates the single actual boot output without selecting another HDMI port. */
static int
output_query(
	void *opaque,
	void *private_session,
	struct gpu_display_info *request)
{
	struct display_device *controller;
	struct bcm2711_display *display;

	/* Inventory is fixed to the output actually adopted during boot. */
	(void)private_session;
	controller = opaque;
	display = controller->native;
	request->count = 1;
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;
	if (request->index != 0)
		return EINVAL;
	request->display_id = display->port + 1U;
	request->generation = 1;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_ACTIVE | GPU_DISPLAY_FIFO;
	request->plane_count = 1;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = DISPLAY_RESOURCE_BYTES;
	request->current_width = display->screen.width;
	request->current_height = display->screen.height;
	request->preferred_width = display->screen.width;
	request->preferred_height = display->screen.height;
	request->max_width = display->screen.width;
	request->max_height = display->screen.height;
	request->refresh_millihz = display->refresh_millihz;
	kern_snprintf(request->name, sizeof(request->name), "BCM2711 HDMI%u", display->port);

	/* Succeeded: the caller sees the preserved firmware mode and native FIFO timing. */
	return 0;
}

/* Enumerates or validates only the existing, physically initialized mode. */
static int
output_mode(
	void *opaque,
	void *private_session,
	struct gpu_display_mode *request)
{
	struct display_device *controller;
	struct drv_bcm2711_boot_screen *screen;
	int error;

	/* Matches stable output identity and generation before describing its only mode. */
	(void)private_session;
	controller = opaque;
	error = find_output(controller, request->display_id, request->generation);
	if (error != 0)
		return error;
	screen = &controller->native->screen;
	request->count = 1;
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		/* Count-only enumeration does not interpret an output ordinal. */
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0)
			return EINVAL;
	} else if (request->operation == GPU_DISPLAY_MODE_VALIDATE) {
		/* No unsupported mode request may reconfigure the preserved boot pipeline. */
		if (request->width != screen->width || request->height != screen->height)
			return ENOTSUP;
		if (request->refresh_millihz != 0 && request->refresh_millihz != controller->native->refresh_millihz)
			return ENOTSUP;
	} else {
		return EINVAL;
	}

	/* Returns the exact native cadence rather than assuming every firmware mode is 60 Hz. */
	request->width = screen->width;
	request->height = screen->height;
	request->refresh_millihz = controller->native->refresh_millihz;
	request->flags = GPU_DISPLAY_FIFO;

	/* Succeeded: enumeration and validation changed no hardware state. */
	return 0;
}

/* Reserves one full-output plane without changing its current scanout. */
static int
output_claim(
	void *opaque,
	void *private_session,
	struct gpu_display_claim *request)
{
	struct display_device *controller;
	struct display_session *session;
	int error;

	/* Serializes output arbitration across independent open descriptions. */
	controller = opaque;
	session = private_session;
	mutex_lock(&controller->mutex);

	error = find_output(controller, request->display_id, request->generation);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Repeated or foreign leases never share the same output owner. */
	if (request->plane_index != 0 || controller->owner != NULL) {
		mutex_unlock(&controller->mutex);
		return EBUSY;
	}

	/* Recovers a previous close before granting ownership of its private targets. */
	error = restore_console(controller);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Lease identifiers never wrap into a previously valid ownership capability. */
	if (controller->next_lease == UINT64_MAX) {
		mutex_unlock(&controller->mutex);
		return EOVERFLOW;
	}

	/* Publishes an independent observation cursor for the new exclusive lease. */
	session->completed = 0;
	session->present_time = 0;
	session->lease = controller->next_lease;
	controller->next_lease++;
	controller->owner = session;
	request->lease = session->lease;

	mutex_unlock(&controller->mutex);

	/* Succeeded: this open alone may submit or release this plane. */
	return 0;
}

/* Restores actual console scanout before consuming the caller's output lease. */
static int
output_release(
	void *opaque,
	void *private_session,
	const struct gpu_display_release *request)
{
	struct display_device *controller;
	struct display_session *session;
	int error;

	/* Keeps a failed release retryable while both native targets remain device-owned. */
	controller = opaque;
	session = private_session;
	mutex_lock(&controller->mutex);

	if (controller->owner != session || request->lease != session->lease) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}

	/* Restores console adoption before changing output ownership. */
	error = restore_console(controller);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Only confirmed restoration gives the plane back to another open. */
	controller->owner = NULL;
	session->lease = 0;

	mutex_unlock(&controller->mutex);

	/* Succeeded: the native console is adopted and the lease is consumed. */
	return 0;
}

/* Copies a complete frame into an unheld private target and waits for native adoption. */
static int
output_present(
	void *opaque,
	void *private_session,
	void *private_resource,
	struct gpu_display_present *request)
{
	struct display_device *controller;
	struct display_session *session;
	struct display_resource *resource;
	struct drv_bcm2711_boot_screen frame;
	int error;

	/* Gives one caller exclusive access to copy-buffer selection and publication. */
	controller = opaque;
	session = private_session;
	resource = private_resource;
	mutex_lock(&controller->mutex);

	if (controller->owner != session || request->lease != session->lease) {
		mutex_unlock(&controller->mutex);
		return EBUSY;
	}

	/* A lease may name only its own storage and the unchanged output generation. */
	if (resource->owner != session || request->generation != 1) {
		mutex_unlock(&controller->mutex);
		return ESTALE;
	}

	/* A completed presentation identity must never wrap into an earlier frame. */
	if (controller->completed == UINT64_MAX) {
		mutex_unlock(&controller->mutex);
		return EOVERFLOW;
	}

	/* Copy targets remain controller-owned even when publication later times out. */
	error = copy_frame(controller, resource, request, &frame);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Publishes the complete copy and waits for its actual current-list adoption. */
	error = bcm2711_display_flip_compose(controller->native, &frame, 0, 0, false);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Completion counts successful presents, independently of raw refresh interrupts. */
	controller->completed++;
	controller->present_time = sched_ticks() * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	session->completed = controller->completed;
	session->present_time = controller->present_time;
	request->sequence = session->completed;

	mutex_unlock(&controller->mutex);

	/* Succeeded: the source resource is no longer borrowed by hardware. */
	return 0;
}

/* Observes already completed synchronous presents belonging to this lease. */
static int
output_wait(
	void *opaque,
	void *private_session,
	struct gpu_display_wait *request)
{
	struct display_device *controller;
	struct display_session *session;

	/* Synchronous present leaves no pending successful sequence to wait for. */
	controller = opaque;
	session = private_session;
	mutex_lock(&controller->mutex);

	if (controller->owner != session || request->lease != session->lease) {
		mutex_unlock(&controller->mutex);
		return EBUSY;
	}

	/* An earlier lease cannot supply the completion requested by this new owner. */
	if (request->sequence > session->completed) {
		mutex_unlock(&controller->mutex);
		return ETIMEDOUT;
	}

	/* Returns the latest confirmed frame rather than the submission cursor. */
	request->completed_sequence = session->completed;
	request->present_time_ns = session->present_time;
	request->generation = 1;

	mutex_unlock(&controller->mutex);

	/* Succeeded: the queried sequence was adopted on this actual output. */
	return 0;
}

/* Supplies the immutable boot-output inventory sequence without waiting on present. */
static int
output_events(
	void *opaque,
	void *private_session,
	uint64_t *sequence)
{
	/* Hotplug and mode changes are not advertised by this initial-mode inventory. */
	(void)opaque;
	(void)private_session;
	*sequence = 1;

	/* Succeeded: a new open can acknowledge this complete stable inventory. */
	return 0;
}

/* Describes a display-only device without claiming a currently registered renderer. */
static int
output_device(
	void *opaque,
	void *private_session,
	struct gpu_device_info *info)
{
	/* The common core supplies device_id; no companion is invented before V3D registers. */
	(void)opaque;
	(void)private_session;
	info->roles = GPU_DEVICE_DISPLAY;
	info->companion_id = 0;
	info->flags = 0;

	/* Succeeded: device selection can distinguish display from render operations. */
	return 0;
}

/* Reports ordinary copy support and the exact native HVS placement requirements. */
static int
output_constraints(
	void *opaque,
	void *private_session,
	struct gpu_scanout_constraints *request)
{
	struct display_device *controller;
	int error;

	/* Constraints belong to the preserved boot output generation. */
	(void)private_session;
	controller = opaque;
	error = find_output(controller, request->display_id, request->generation);
	if (error != 0)
		return error;
	request->flags = GPU_SCANOUT_COPY;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->stride_alignment = 4;
	request->offset_alignment = 4;
	request->placement = GPU_PLACEMENT_CONTIGUOUS;
	request->max_dma_address = 0x3fffffffU;

	/* Succeeded: no foreign or shared zero-copy import is promised by this table. */
	return 0;
}

/* Resolves only the stable identifier and generation of the active boot port. */
static int
find_output(
	const struct display_device *controller,
	uint32_t identifier,
	uint64_t generation)
{
	/* A different physical output cannot be selected by a stale or arbitrary identifier. */
	if (identifier != controller->native->port + 1U)
		return ENXIO;
	if (generation != 1)
		return ESTALE;

	/* Succeeded: the request names the unique initialized boot output. */
	return 0;
}

/* Restores retained console bytes only when private DMA or uncertainty requires it. */
static int
restore_console(
	struct display_device *controller)
{
	struct bcm2711_flip_status status;
	int error;

	/* A confirmed unheld console needs no redundant hardware transaction. */
	bcm2711_display_flip_snapshot(controller->native, &status);
	if (!status.uncertain && status.retained_mask == 0)
		return 0;

	/* An uncertain restore keeps both private allocations alive in the controller. */
	error = bcm2711_display_flip_restore(controller->native);
	if (error != 0)
		return error;

	/* Succeeded: another lease may reuse the private copy buffers. */
	return 0;
}

/* Converts one packed frame into an inactive device-owned target without scaling. */
static int
copy_frame(
	struct display_device *controller,
	const struct display_resource *resource,
	const struct gpu_display_present *request,
	struct drv_bcm2711_boot_screen *frame)
{
	struct bcm2711_flip_status status;
	struct bcm2711_buffer *target;
	const uint8_t *source;
	uint8_t *destination;
	uint64_t span;
	uint32_t native_format;
	uint32_t slot;
	uint32_t row;
	uint32_t column;

	/* Supports complete frames in either defined byte order, without BLOB borrowing. */
	if (request->flags != GPU_DISPLAY_PRESENT_FIFO)
		return ENOTSUP;
	if (request->width != controller->native->screen.width || request->height != controller->native->screen.height)
		return ENOTSUP;
	if (request->refresh_millihz != 0 && request->refresh_millihz != controller->native->refresh_millihz)
		return ENOTSUP;
	if (request->format != GPU_PIXEL_BGRA8888 && request->format != GPU_PIXEL_RGBA8888)
		return ENOTSUP;
	if (request->stride < request->width * 4U || (request->stride & 3U) != 0)
		return EINVAL;
	if ((request->offset & 3U) != 0)
		return EINVAL;
	span = (uint64_t)(request->height - 1U) * request->stride + request->width * 4U;
	if (request->offset > resource->buffer->bytes || span > resource->buffer->bytes - request->offset)
		return EINVAL;

	/* DMA holds select the inactive target rather than a CPU-only alternation counter. */
	bcm2711_display_flip_snapshot(controller->native, &status);
	if (status.uncertain || status.busy)
		return EBUSY;
	slot = 0;
	if ((status.retained_mask & 1U) != 0)
		slot = 1;
	target = controller->copies[slot];
	native_format = GPU_PIXEL_BGRA8888;
	if (controller->native->screen.format == 1)
		native_format = GPU_PIXEL_RGBA8888;

	/* Copies active pixels row by row, preserving the native target's padded pitch. */
	for (row = 0; row < request->height; row++) {
		/* Row bounds were established before selecting any driver-owned target. */
		source = (const uint8_t *)resource->buffer->address + request->offset + (uint64_t)row * request->stride;
		destination = (uint8_t *)target->address + (uint64_t)row * controller->native->screen.pitch;
		if (native_format == request->format) {
			/* Native byte order allows one exact active-row copy. */
			kern_memcpy(destination, source, request->width * 4U);
		} else {
			/* Exchanges red and blue without changing green or alpha. */
			for (column = 0; column < request->width; column++) {
				/* Every destination pixel preserves its two unchanged components. */
				destination[column * 4U] = source[column * 4U + 2U];
				destination[column * 4U + 1U] = source[column * 4U + 1U];
				destination[column * 4U + 2U] = source[column * 4U];
				destination[column * 4U + 3U] = source[column * 4U + 3U];
			}
		}
	}

	/* Produces the unchanged-mode native descriptor; flip owns cache publication. */
	*frame = controller->native->screen;
	frame->physical = target->memory.paddr;

	/* Succeeded: only a confirmed inactive private target contains the candidate frame. */
	return 0;
}

/* Binds complete operations before the common GPU core publishes any open. */
static void
bind_operations(
	struct display_device *controller)
{
	/* Connects complete discovery, lease, FIFO completion and inventory operations. */
	controller->display_operations.query = output_query;
	controller->display_operations.mode = output_mode;
	controller->display_operations.claim = output_claim;
	controller->display_operations.release = output_release;
	controller->display_operations.present = output_present;
	controller->display_operations.wait = output_wait;
	controller->display_operations.events = output_events;

	/* Publishes pairing and constraints without unsupported native imports. */
	controller->scanout_operations.query_device = output_device;
	controller->scanout_operations.constraints = output_constraints;

	/* Describes actual storage, transfers, mappings and display support. */
	controller->operations.version = DRV_GPU_INTERFACE_VERSION;
	controller->operations.size = sizeof(controller->operations);
	controller->operations.capabilities = DISPLAY_CAPABILITIES;
	controller->operations.open = device_open;
	controller->operations.close = device_close;
	controller->operations.get_info = device_info;
	controller->operations.resource_create = resource_create;
	controller->operations.resource_destroy = resource_destroy;
	controller->operations.resource_read = resource_read;
	controller->operations.resource_write = resource_write;
	controller->operations.resource_map = resource_map;
	controller->operations.display = &controller->display_operations;
	controller->operations.scanout = &controller->scanout_operations;
}
