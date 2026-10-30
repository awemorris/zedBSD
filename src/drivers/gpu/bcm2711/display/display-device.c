/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native display leases own copy targets and independent shared scanout holds. */
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <uapi/errno.h>
#include <uapi/gpu-allocation.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/share.h"

/* Resource limits describe actual ordinary packed-pixel storage support. */
#define DISPLAY_RESOURCE_BYTES (32ULL * 1024U * 1024U)
#define DISPLAY_RESOURCE_COUNT 64U
#define DISPLAY_CAPABILITIES (GPU_CAP_RESOURCE | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS | GPU_CAP_BLOB | GPU_CAP_SHARE | GPU_CAP_ALLOCATION_SHARE)

/* One open owns its resources and at most one nontransferrable display lease. */
struct display_session {
	uint64_t lease;
	uint32_t resources;
	uint32_t next_resource;
};

/* One open owns its descriptor; independent native holds keep its allocation alive. */
struct display_resource {
	struct display_session *owner;
	struct bcm2711_buffer *buffer;
	struct gpu_image_descriptor image;
	bool blob;
	bool shareable;
	bool mappable;
	bool has_image;
};

/*
 * The cursor plane keeps five images: the three lists the hardware may read
 * (the adopted one, one published and one replacing it) may each show a
 * different image, the recorded cursor may name a fourth not yet in a list,
 * and a fifth is always free to write.
 */
#define DISPLAY_CURSOR_IMAGES 5U

/* The source of one cursor image's rows; valid is false when the image holds nothing reusable. */
struct display_cursor_key {
	bool valid;
	uint64_t identity;
	uint64_t offset;
	uint64_t generation;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
};

/*
 * One boot output owns persistent operation tables and private scanout buffers.
 *
 * mutex orders lease changes, the copy path and resource bookkeeping;
 * presentation of shared images and the cursor do not take it (the flip
 * state checks the lease under its own guard).  plane_mutex serializes the
 * cursor's images and is taken after mutex.  The numbers presents and
 * commits return are the flip state's record numbers (one count for the
 * output that never goes back).
 */
struct display_device {
	struct mutex mutex;
	struct mutex plane_mutex;
	struct bcm2711_display *native;
	struct drv_gpu_device *gpu;
	struct drv_gpu_ops operations;
	struct drv_gpu_display_ops display_operations;
	struct drv_gpu_scanout_ops scanout_operations;
	struct drv_gpu_share_ops share_operations;
	struct bcm2711_buffer *copies[2];

	/* The cursor plane's images, made at the first cursor image (plane_mutex). */
	struct bcm2711_buffer *cursors[DISPLAY_CURSOR_IMAGES];
	struct bcm2711_flip_cursor cursor;

	/*
	 * What each cursor image holds: the source run's identity, the rows'
	 * place and format, and the run's generation read before the copy.  A
	 * SET of the same unchanged rows shows that image without copying.  The
	 * plane mutex protects them; a lease change forgets them.
	 */
	struct display_cursor_key keys[DISPLAY_CURSOR_IMAGES];
	struct display_session *owner;
	uint64_t next_lease;
	bool registered;
};

/* The node and its borrowed tables persist for the entire kernel lifetime. */
static struct display_device display_controller;

static int device_open(void *opaque, void **result);
static void device_close(void *opaque, void *private_session);
static int device_info(void *opaque, void *private_session, struct gpu_info *info);
static int resource_create(void *opaque, void *private_session, const struct gpu_resource_create *request, void **result);
static void resource_destroy(void *opaque, void *private_session, void *private_resource);
static void display_hold(struct bcm2711_buffer *buffer, bool hold);
static int resource_read(void *opaque, void *private_session, void *private_resource, uint64_t offset, void *data, uint32_t bytes);
static int resource_write(void *opaque, void *private_session, void *private_resource, uint64_t offset, const void *data, uint32_t bytes);
static int blob_create(void *opaque, void *private_session, const struct gpu_blob_create *request, void **result, uint32_t *identifier);
static int blob_create_placed(void *opaque, void *private_session, const struct gpu_blob_create_placed *request, void **result, uint32_t *identifier);
static int allocate_blob(struct display_device *controller, struct display_session *session, const struct gpu_blob_create *request, const struct gpu_placement *placement, void **result, uint32_t *identifier);
static int export_resource(void *opaque, void *private_session, void *private_resource, const struct gpu_image_descriptor *image, void **result);
static void release_shared(void *opaque, void *private_shared);
static int import_resource(void *opaque, void *private_session, void *private_shared, uint32_t flags, void **result, uint32_t *identifier);
static int export_backing(void *opaque, void *private_shared, struct drv_gpu_scanout_backing *backing);
static int import_image(void *opaque, void *private_session, const struct gpu_image_descriptor *image, const struct drv_gpu_scanout_backing *backing, void **result);
static int retain_resource(struct display_device *controller, struct display_session *session, struct bcm2711_buffer *buffer, const struct gpu_image_descriptor *image, void **result);
static int shared_frame(struct display_device *controller, const struct display_resource *resource, const struct gpu_display_present *request, struct drv_bcm2711_boot_screen *frame);
static int copy_present(struct display_device *controller, struct display_session *session, const struct display_resource *resource, struct gpu_display_present *request);
static int output_adoption_wait(void *opaque, void *private_session, uint64_t lease, uint64_t *sequence, uint64_t *adopted);
static void lease_owner(struct display_session *session, uint64_t lease, struct bcm2711_flip_owner *owner);
static int output_commit(void *opaque, void *private_session, void *const *objects, struct gpu_display_commit *request);
static int commit_change(struct display_device *controller, struct display_session *session, void *object, const struct gpu_display_plane_state *state, struct bcm2711_flip_change *change);
static int output_overlay_info(void *opaque, void *private_session, struct gpu_display_overlay_info *request);
static int output_cursor_set(void *opaque, void *private_session, void *private_resource, const struct gpu_display_cursor_image *request);
static int output_cursor_move(void *opaque, void *private_session, const struct gpu_display_cursor_move *request);
static int output_cursor_hide(void *opaque, void *private_session, const struct gpu_display_cursor_hide *request);
static int cursor_owner(struct display_device *controller, struct display_session *session, uint64_t lease, uint64_t generation);
static int cursor_record(struct display_device *controller, struct display_session *session, uint64_t lease, const struct bcm2711_flip_cursor *cursor);
static void cursor_reset(struct display_device *controller);
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
static int copy_frame(struct display_device *controller, struct display_session *session, const struct display_resource *resource, const struct gpu_display_present *request, struct drv_bcm2711_boot_screen *frame);
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
	error = mutex_init(&controller->plane_mutex, LOCK_RANK_DEVICE, "bcm2711-display-cursor");
	if (error != 0)
		return error;
	controller->native = display;
	controller->next_lease = 1;

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
	struct bcm2711_flip_owner owner;
	int revoked;
	int error;

	/* Ends the lease first, so a present waiting for its adoption returns instead of delaying the restore. */
	controller = opaque;
	session = private_session;
	lease_owner(session, session->lease, &owner);
	revoked = bcm2711_display_flip_revoke(controller->native, &owner);

	/* Stops referring to the closing session even if DMA retirement cannot be proved. */
	mutex_lock(&controller->mutex);

	if (revoked == 0 && controller->owner == session) {
		/* The lease's cursor goes with it; copy targets belong to the permanent controller, never to this session. */
		cursor_reset(controller);
		error = restore_console(controller);
		if (error != 0)
			bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "lease close retains uncertain copy buffers (%d)", error);
		controller->owner = NULL;
		session->lease = 0;
		bcm2711_display_flip_disown(controller->native);

		/* An uncertain native restore cannot safely return framebuffer writes to text. */
		if (error == 0)
			kern_text_resume();
	}

	mutex_unlock(&controller->mutex);

	/* The common core retired every resource/VM pin before invoking final close. */
	kern_free(session);

	/* Succeeded: the closed display session owns no remaining public resource metadata. */
	return;
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
	display_hold(resource->buffer, true);
	resource->owner = session;
	resource->mappable = true;
	session->resources++;
	*result = resource;

	mutex_unlock(&controller->mutex);

	/* Succeeded: common resource teardown owns the descriptor and its initial reference. */
	return 0;
}

/* Retires a descriptor after pins; native DMA holds own separate allocation references. */
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
	display_hold(resource->buffer, false);
	bcm2711_buffer_release(resource->buffer);

	mutex_unlock(&controller->mutex);

	/* Only common resource pins kept this descriptor alive. */
	kern_free(resource);

	/* Succeeded: the withdrawn display identity no longer holds its backing allocation. */
	return;
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

/* Copies bytes into retained native RAM; Vulkan fences arbitrate shared accesses. */
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
	unsigned long enabled;

	/* Uses immutable allocation bounds to reject a truncated transfer. */
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session)
		return EINVAL;
	if (offset > resource->buffer->bytes || bytes > resource->buffer->bytes - offset)
		return EINVAL;
	mutex_lock(&controller->mutex);

	kern_memcpy((uint8_t *)resource->buffer->address + offset, data, bytes);

	/*
	 * An unrecorded generation step: snapshots go stale and the next copy
	 * from this run is whole.  The run's guard orders it with the render
	 * device's writes, which do not take this mutex.
	 */
	enabled = spin_lock_irqsave(&resource->buffer->guard);

	resource->buffer->generation++;

	spin_unlock_irqrestore(&resource->buffer->guard, enabled);

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
	if (!resource->mappable)
		return ENOTSUP;

	/* A shared run mapped here can change without a generation step, so render snapshots stop trusting it. */
	resource->buffer->host_mapped = true;
	mapping->physical = resource->buffer->memory.paddr;
	mapping->address = resource->buffer->address;
	mapping->bytes = resource->buffer->bytes;
	mapping->attributes = 0;

	/* Every user mapping preserves the allocation's immutable CPU cache policy. */
	if (resource->buffer->uncached)
		mapping->attributes = DRV_GPU_MAPPING_UNCACHED_RAM;

	/* Succeeded: the mapping describes managed RAM with its owner's CPU cache policy. */
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
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_ACTIVE | GPU_DISPLAY_FIFO | GPU_DISPLAY_HAS_CURSOR | GPU_DISPLAY_HAS_OVERLAYS;
	request->plane_count = 1U + BCM2711_FLIP_OVERLAYS;
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
	struct bcm2711_flip_owner owner;
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

	/* A new lease starts without a cursor image. */
	cursor_reset(controller);

	/* Publishes an independent observation cursor for the new exclusive lease. */
	session->lease = controller->next_lease;
	controller->next_lease++;
	controller->owner = session;

	/* The flip state checks the lease from now on, without this mutex. */
	lease_owner(session, session->lease, &owner);
	bcm2711_display_flip_own(controller->native, &owner);

	/* Stops console pixel writes before this lease may publish a graphics frame. */
	kern_text_suspend();
	request->lease = session->lease;

	mutex_unlock(&controller->mutex);

	/*
	 * Each lease's start is logged, so the order of a display's leases
	 * against other drivers' lines (the firmware's video decoder)
	 * can be read from the kernel log; leases change rarely.
	 */
	kern_logf("bcm2711: display lease %llu claimed\n", (unsigned long long)request->lease);

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
	struct bcm2711_flip_owner owner;
	uint64_t started;
	uint64_t restored;
	uint64_t finished;
	int error;

	/*
	 * Ends the lease first: its records fail from now on and a present
	 * waiting for an adoption returns, before this takes the mutex.
	 */
	controller = opaque;
	session = private_session;
	lease_owner(session, request->lease, &owner);
	error = bcm2711_display_flip_revoke(controller->native, &owner);
	if (error != 0)
		return EINVAL;

	/* Keeps a failed release retryable while both native targets remain device-owned. */
	started = sched_ticks();
	mutex_lock(&controller->mutex);

	if (controller->owner != session || request->lease != session->lease) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}

	/* The lease's cursor goes with it, after any cursor change in progress. */
	cursor_reset(controller);

	/* Restores console adoption before changing output ownership. */
	error = restore_console(controller);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Only confirmed restoration gives the plane back to another open. */
	controller->owner = NULL;
	session->lease = 0;
	bcm2711_display_flip_disown(controller->native);

	/* Repaints retained text only after actual console scanout is confirmed. */
	restored = sched_ticks();
	kern_text_resume();

	mutex_unlock(&controller->mutex);

	/* The lease's end, with how long the console's return and its text took (see output_claim). */
	finished = sched_ticks();
	kern_logf("bcm2711: display lease %llu released restore_ms=%llu text_ms=%llu\n",
		  (unsigned long long)request->lease,
		  (unsigned long long)kern_ticks_to_ms(restored - started),
		  (unsigned long long)kern_ticks_to_ms(finished - restored));

	/* Succeeded: the native console is adopted and the lease is consumed. */
	return 0;
}

/*
 * Records a frame of copied storage or of an independently retained blob, without waiting.
 *
 * The core then calls output_adoption_wait with the same request, after it
 * has unpinned the resource: sequence carries the record's number until then.
 */
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
	struct bcm2711_flip_primary primary;
	struct bcm2711_flip_owner owner;
	uint64_t serial;
	int error;

	/* Only the lease's own open presents, on the output it leases. */
	controller = opaque;
	session = private_session;
	resource = private_resource;
	lease_owner(session, request->lease, &owner);
	error = bcm2711_display_flip_owned(controller->native, &owner);
	if (error != 0)
		return EBUSY;

	/* A lease may name only its own storage and the unchanged output generation. */
	if (resource->owner != session || request->generation != 1)
		return ESTALE;

	/* Ordinary storage is copied into a private target under the controller mutex. */
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) == 0) {
		error = copy_present(controller, session, resource, request);
		if (error != 0)
			return error;

		/* Succeeded: the copied frame is recorded; its adoption is waited for next. */
		return 0;
	}

	/* A native blob is shown directly from its allocation. */
	error = shared_frame(controller, resource, request, &frame);
	if (error != 0)
		return error;

	/* The recorded plane borrows the allocation with its own reference. */
	kern_memset(&primary, 0, sizeof(primary));
	primary.shown = true;
	primary.frame = frame;
	primary.hold = resource->buffer;
	bcm2711_buffer_retain(resource->buffer);
	error = bcm2711_display_flip_record(controller->native, &owner, &primary, &serial);
	if (error != 0) {
		bcm2711_buffer_release(resource->buffer);
		return error;
	}

	/* The core passes the record's number to the adoption wait. */
	request->sequence = serial;

	/* Succeeded: the frame is recorded and its list published or carried by the next one. */
	return 0;
}

/*
 * Waits until a present's or commit's number is adopted (GPU_DISPLAY_WAIT).
 *
 * Numbers are the output's record numbers, shared by every lease; one call
 * waits at most a second, and a signal ends it.
 */
static int
output_wait(
	void *opaque,
	void *private_session,
	struct gpu_display_wait *request)
{
	struct display_device *controller;
	struct display_session *session;
	struct bcm2711_flip_owner owner;
	uint64_t timeout;
	uint64_t adopted;
	uint64_t adopted_ticks;
	int error;

	/* At most a second, in scheduler ticks, rounding a partial tick up. */
	controller = opaque;
	session = private_session;
	timeout = request->timeout_ns;
	if (timeout > KERN_NSEC_PER_SEC)
		timeout = KERN_NSEC_PER_SEC;
	timeout = kern_ns_to_ticks(timeout);

	/* Sleeps for the adoption without any mutex; only the lease's own open waits. */
	lease_owner(session, request->lease, &owner);
	error = bcm2711_display_flip_wait_number(controller->native, &owner, request->sequence, timeout, &adopted, &adopted_ticks);
	if (error != 0)
		return error;

	/* The highest number adopted with every lower one, and the time of that adoption. */
	request->completed_sequence = adopted;
	request->present_time_ns = kern_ticks_to_ns(adopted_ticks);
	request->generation = 1;

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
	request->flags = GPU_SCANOUT_COPY | GPU_SCANOUT_SHARED | GPU_SCANOUT_FOREIGN;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->stride_alignment = 4;
	request->offset_alignment = 4;
	request->placement = GPU_PLACEMENT_CONTIGUOUS;
	request->max_dma_address = 0x3fffffffU;

	/* Succeeded: native allocation imports must satisfy every listed placement requirement. */
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

/* Restores retained console bytes only when lists, borrowed images or uncertainty require it. */
static int
restore_console(
	struct display_device *controller)
{
	struct bcm2711_flip_status status;
	int error;

	/* A confirmed console that names nothing needs no hardware transaction. */
	bcm2711_display_flip_snapshot(controller->native, &status);
	if (!status.uncertain &&
	    status.active_list == 43U &&
	    status.pending_count == 0 &&
	    status.primary.hold == NULL &&
	    status.retiring_count == 0)
		return 0;

	/* An uncertain restore keeps private targets and shared allocations alive in the flip state. */
	error = bcm2711_display_flip_restore(controller->native);
	if (error != 0)
		return error;

	/* Succeeded: another lease may reuse the private copy buffers. */
	return 0;
}

/*
 * Converts one packed frame into a private target no list the hardware may read names.
 *
 * The caller holds the controller mutex, so no other copy names a target
 * meanwhile.  A target only a list names becomes free at the next adoption,
 * which this waits for (at most one refresh, bounded at 100 ms).
 */
static int
copy_frame(
	struct display_device *controller,
	struct display_session *session,
	const struct display_resource *resource,
	const struct gpu_display_present *request,
	struct drv_bcm2711_boot_screen *frame)
{
	struct bcm2711_flip_owner owner;
	struct bcm2711_buffer *target;
	struct bcm2711_buffer *listed;
	const uint8_t *source;
	uint8_t *destination;
	uint64_t span;
	uint32_t native_format;
	uint32_t index;
	uint32_t row;
	uint32_t column;
	unsigned use;
	int error;

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

	/*
	 * Picks a target nothing names; failing that, one only lists name (the
	 * recorded frame stays named until this very present replaces it).
	 */
	target = NULL;
	listed = NULL;
	for (index = 0; index < 2U; index++) {
		/* What names this target now. */
		use = bcm2711_display_flip_frame_use(controller->native, controller->copies[index]->memory.paddr, controller->copies[index]->bytes);
		if (use == BCM2711_FLIP_FRAME_FREE && target == NULL)
			target = controller->copies[index];
		if (use == BCM2711_FLIP_FRAME_LISTED && listed == NULL)
			listed = controller->copies[index];
	}

	/* A target only lists name is free once a later list is adopted. */
	if (target == NULL && listed != NULL) {
		lease_owner(session, request->lease, &owner);
		error = bcm2711_display_flip_wait_unlisted(controller->native, &owner, listed->memory.paddr, listed->bytes);
		if (error != 0)
			return error;
		target = listed;
	}

	/* Both targets are named by recorded frames: the copy cannot go anywhere now. */
	if (target == NULL)
		return EBUSY;
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

	/* Succeeded: only a target no readable list names contains the candidate frame. */
	return 0;
}

/*
 * Copies ordinary storage into a private target and records it, under the controller mutex.
 *
 * The mutex keeps two copies from choosing the same target; the record does
 * not wait for its adoption, which the core waits for after this returns.
 */
static int
copy_present(
	struct display_device *controller,
	struct display_session *session,
	const struct display_resource *resource,
	struct gpu_display_present *request)
{
	struct bcm2711_flip_primary primary;
	struct bcm2711_flip_owner owner;
	uint64_t serial;
	int error;

	/* One copy at a time chooses and fills a target. */
	mutex_lock(&controller->mutex);

	kern_memset(&primary, 0, sizeof(primary));
	error = copy_frame(controller, session, resource, request, &primary.frame);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* The private target belongs to the controller, so the record borrows nothing. */
	primary.shown = true;
	lease_owner(session, request->lease, &owner);
	error = bcm2711_display_flip_record(controller->native, &owner, &primary, &serial);

	mutex_unlock(&controller->mutex);

	/* A refused record leaves the target unnamed. */
	if (error != 0)
		return error;

	/* The core passes the record's number to the adoption wait. */
	request->sequence = serial;

	/* Succeeded: the copied frame is recorded. */
	return 0;
}

/*
 * Waits for the adoption of a recorded present or commit (the ungated core's second step).
 *
 * sequence holds the record's number, which is also the number the caller
 * sees; adopted, when given, receives the highest adopted number.
 */
static int
output_adoption_wait(
	void *opaque,
	void *private_session,
	uint64_t lease,
	uint64_t *sequence,
	uint64_t *adopted)
{
	struct display_device *controller;
	struct display_session *session;
	struct bcm2711_flip_owner owner;
	struct bcm2711_flip_status status;
	uint64_t mark;
	uint64_t wait_ticks;
	int error;

	/* Waits for a list showing the record, holding no lock or pin. */
	controller = opaque;
	session = private_session;
	lease_owner(session, lease, &owner);
	mark = clock_ticks();
	error = bcm2711_display_flip_wait(controller->native, &owner, *sequence);
	wait_ticks = clock_ticks() - mark;
	if (error != 0)
		return error;

	/* The adoption is a cadence event and a flip of the report. */
	bcm2711_display_flip_note(controller->native, BCM2711_FLIP_CADENCE_FLIP);
	bcm2711_display_flip_report(controller->native, true, 0, 0, wait_ticks);

	/* The highest adopted number, at least the record's own. */
	if (adopted != NULL) {
		bcm2711_display_flip_snapshot(controller->native, &status);
		*adopted = status.adopted_serial;
	}

	/* Succeeded: the record is shown. */
	return 0;
}

/*
 * Records some planes of the leased output at once (GPU_DISPLAY_COMMIT), without waiting.
 *
 * Each named image is a native blob of this open, borrowed with its own
 * reference; a dropped plane's reference goes back at once.  The core waits
 * for the adoption afterwards when the commit asks.
 */
static int
output_commit(
	void *opaque,
	void *private_session,
	void *const *objects,
	struct gpu_display_commit *request)
{
	struct display_device *controller;
	struct display_session *session;
	struct bcm2711_flip_owner owner;
	struct bcm2711_flip_change changes[BCM2711_FLIP_PLANES];
	uint64_t sequence;
	uint64_t adopted;
	uint32_t index;
	int error;

	/* Only the lease's own open changes its planes, on the output generation it leases. */
	controller = opaque;
	session = private_session;
	lease_owner(session, request->lease, &owner);
	error = bcm2711_display_flip_owned(controller->native, &owner);
	if (error != 0)
		return EBUSY;
	if (request->generation != 1)
		return ESTALE;
	if (request->count > BCM2711_FLIP_PLANES)
		return EINVAL;

	/* Describes each plane's new state, taking a reference to each named image. */
	kern_memset(changes, 0, sizeof(changes));
	for (index = 0; index < request->count; index++) {
		/* The plane's flags and rectangles; a named image is described below. */
		error = commit_change(controller, session, objects[index], &request->planes[index], &changes[index]);
		if (error != 0)
			break;
	}

	/* Records every plane at once, or none. */
	if (error == 0)
		error = bcm2711_display_flip_commit(controller->native, &owner, changes, request->count, &sequence, &adopted);

	/* A refused commit, or a dropped plane, gives its image's reference back. */
	for (index = 0; index < request->count; index++) {
		/* Only a described image holds a reference. */
		if (changes[index].hold == NULL)
			continue;
		if (error != 0 || !changes[index].recorded)
			bcm2711_buffer_release(changes[index].hold);
	}

	/* Reports the refused commit. */
	if (error != 0)
		return error;

	/* Each plane's result, the record's number and the adopted number. */
	for (index = 0; index < request->count; index++) {
		/* A recorded state, or one a later serial overtook. */
		request->planes[index].result = GPU_DISPLAY_PLANE_DROPPED;
		if (changes[index].recorded)
			request->planes[index].result = GPU_DISPLAY_PLANE_APPLIED;
	}

	/* The numbers the caller waits with. */
	request->sequence = sequence;
	request->adopted = adopted;

	/* Succeeded: the planes show from the refresh boundary that adopts a list with them. */
	return 0;
}

/* Describes one plane's state of a commit, taking a reference to a named image. */
static int
commit_change(
	struct display_device *controller,
	struct display_session *session,
	void *object,
	const struct gpu_display_plane_state *state,
	struct bcm2711_flip_change *change)
{
	struct display_resource *resource;
	uint64_t bytes;

	/* The plane, its serial, whether it shows and its rectangles. */
	(void)controller;
	change->plane = state->plane;
	change->serial = state->serial;
	if ((state->flags & GPU_DISPLAY_PLANE_SHOWN) != 0U)
		change->shown = true;
	change->src_x = state->src_x;
	change->src_y = state->src_y;
	change->src_width = state->src_width;
	change->src_height = state->src_height;
	change->dst_x = state->dst_x;
	change->dst_y = state->dst_y;
	change->dst_width = state->dst_width;
	change->dst_height = state->dst_height;

	/* A plane keeping its image names no storage. */
	if ((state->flags & GPU_DISPLAY_PLANE_IMAGE) == 0U)
		return 0;

	/* A native blob of this open, of the layout it was imported with. */
	resource = object;
	if (resource == NULL || resource->owner != session)
		return ESTALE;
	if (!resource->blob)
		return EINVAL;
	if (state->stride > 65535U)
		return EINVAL;
	bytes = (uint64_t)state->stride * state->height;
	if (state->offset > resource->buffer->bytes || bytes > resource->buffer->bytes - state->offset)
		return EINVAL;
	if (resource->has_image) {
		if (state->width != resource->image.width ||
		    state->height != resource->image.height ||
		    state->format != resource->image.format ||
		    state->stride != resource->image.stride ||
		    state->offset != resource->image.offset)
			return EINVAL;
	}

	/* The image as the flip state reads it. */
	change->image = true;
	change->frame = controller->native->screen;
	change->frame.physical = resource->buffer->memory.paddr + state->offset;
	change->frame.size = bytes;
	change->frame.width = state->width;
	change->frame.height = state->height;
	change->frame.pitch = state->stride;
	change->frame.uncached = resource->buffer->uncached;
	change->frame.format = 0;
	if (state->format == GPU_PIXEL_RGBA8888)
		change->frame.format = 1;

	/* The state borrows the allocation with a reference of its own. */
	bcm2711_buffer_retain(resource->buffer);
	change->hold = resource->buffer;

	/* Succeeded: the change names its image. */
	return 0;
}

/* Reports an overlay plane's limits on the current mode (GPU_DISPLAY_OVERLAY_INFO). */
static int
output_overlay_info(
	void *opaque,
	void *private_session,
	struct gpu_display_overlay_info *request)
{
	struct display_device *controller;
	int error;

	/* The boot output of its generation, and one of its overlays. */
	(void)private_session;
	controller = opaque;
	error = find_output(controller, request->display_id, request->generation);
	if (error != 0)
		return error;
	if (request->plane == 0 || request->plane > BCM2711_FLIP_OVERLAYS)
		return EINVAL;

	/*
	 * A part of an image as large as the output, scaled up or down on
	 * either axis, shown anywhere inside it.  A vertically scaled overlay's
	 * line buffer holds 2048 pixels, so the source is no wider than that
	 * when the overlay may scale (an unscaled one may be wider; the commit
	 * checks each case).
	 */
	request->max_src_width = controller->native->screen.width;
	if (request->max_src_width > 2048U)
		request->max_src_width = 2048U;
	request->max_src_height = controller->native->screen.height;
	request->max_dst_width = controller->native->screen.width;
	request->max_dst_height = controller->native->screen.height;
	request->flags = GPU_DISPLAY_OVERLAY_SCALE_UP | GPU_DISPLAY_OVERLAY_SCALE_DOWN_X | GPU_DISPLAY_OVERLAY_SCALE_DOWN_Y;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;

	/* Succeeded: the overlay's limits until the mode changes. */
	return 0;
}

/* Describes an open's lease as the flip state's owner. */
static void
lease_owner(
	struct display_session *session,
	uint64_t lease,
	struct bcm2711_flip_owner *owner)
{
	/* The open and the lease it names together. */
	owner->session = session;
	owner->lease = lease;
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
	controller->display_operations.cursor_set = output_cursor_set;
	controller->display_operations.cursor_move = output_cursor_move;
	controller->display_operations.cursor_hide = output_cursor_hide;
	controller->display_operations.adoption_wait = output_adoption_wait;
	controller->display_operations.flags = DRV_GPU_DISPLAY_UNGATED;
	controller->display_operations.commit = output_commit;
	controller->display_operations.overlay_info = output_overlay_info;

	/* Publishes checked foreign native imports with actual physical constraints. */
	controller->scanout_operations.query_device = output_device;
	controller->scanout_operations.constraints = output_constraints;
	controller->scanout_operations.import_image = import_image;

	/* Export capabilities retain RAM independently of the original resource and open. */
	controller->share_operations.export_resource = export_resource;
	controller->share_operations.release = release_shared;
	controller->share_operations.import_resource = import_resource;
	controller->share_operations.get_scanout_backing = export_backing;

	/* Describes actual storage, transfers, mappings and display support. */
	controller->operations.version = DRV_GPU_INTERFACE_VERSION;
	controller->operations.size = sizeof(controller->operations);
	controller->operations.capabilities = DISPLAY_CAPABILITIES;
	controller->operations.open = device_open;
	controller->operations.close = device_close;
	controller->operations.get_info = device_info;
	controller->operations.resource_create = resource_create;
	controller->operations.resource_destroy = resource_destroy;
	controller->operations.blob_create = blob_create;
	controller->operations.blob_create_placed = blob_create_placed;
	controller->operations.resource_read = resource_read;
	controller->operations.resource_write = resource_write;
	controller->operations.resource_map = resource_map;
	controller->operations.display = &controller->display_operations;
	controller->operations.scanout = &controller->scanout_operations;
	controller->operations.share = &controller->share_operations;

	/* Succeeded: the display publishes only its implemented operation groups. */
	return;
}

/* Creates one ordinary native blob without additional placement restrictions. */
static int
blob_create(
	void *opaque,
	void *private_session,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *identifier)
{
	int error;

	/* The common resource wrapper needs a distinct nonzero local blob identity. */
	error = allocate_blob(opaque, private_session, request, NULL, result, identifier);
	if (error != 0)
		return error;

	/* Succeeded: this open owns the created native blob. */
	return 0;
}

/* Creates a native blob whose actual allocation satisfies every accepted condition. */
static int
blob_create_placed(
	void *opaque,
	void *private_session,
	const struct gpu_blob_create_placed *request,
	void **result,
	uint32_t *identifier)
{
	int error;

	/* The shared allocator refuses coherence and verifies actual returned placement. */
	error = allocate_blob(opaque, private_session, &request->blob, &request->placement, result, identifier);
	if (error != 0)
		return error;

	/* Succeeded: the immutable native allocation meets the requested placement. */
	return 0;
}

/* Reserves descriptor capacity before creating the independently owned blob storage. */
static int
allocate_blob(
	struct display_device *controller,
	struct display_session *session,
	const struct gpu_blob_create *request,
	const struct gpu_placement *placement,
	void **result,
	uint32_t *identifier)
{
	struct display_resource *resource;
	int error;

	/* Display blobs share the ordinary resource count and byte limits. */
	*result = NULL;
	*identifier = 0;
	if (request->bytes > DISPLAY_RESOURCE_BYTES)
		return ENOTSUP;
	mutex_lock(&controller->mutex);

	if (session->resources >= DISPLAY_RESOURCE_COUNT || session->next_resource == UINT32_MAX) {
		mutex_unlock(&controller->mutex);
		return ENOSPC;
	}

	/* A descriptor exists before physical allocation can succeed. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL) {
		mutex_unlock(&controller->mutex);
		return ENOMEM;
	}

	/* The placement helper acquires exactly one reference on successful native allocation. */
	error = bcm2711_blob_allocate(request, placement, &resource->buffer);
	if (error != 0) {
		kern_free(resource);
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Publishes immutable blob semantics with a never-reused per-open identifier. */
	display_hold(resource->buffer, true);
	resource->owner = session;
	resource->blob = true;
	if ((request->flags & GPU_BLOB_SHAREABLE) != 0)
		resource->shareable = true;
	if ((request->flags & GPU_BLOB_MAPPABLE) != 0)
		resource->mappable = true;
	session->next_resource++;
	session->resources++;
	*identifier = session->next_resource;
	*result = resource;

	mutex_unlock(&controller->mutex);

	/* Succeeded: the common wrapper owns the complete native blob descriptor. */
	return 0;
}

/* Exports one allocation without exposing a borrowed session descriptor. */
static int
export_resource(
	void *opaque,
	void *private_session,
	void *private_resource,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct display_device *controller;
	struct display_resource *resource;
	struct bcm2711_shared *shared;
	int error;

	/* Only explicitly shareable blobs grant independent allocation capabilities. */
	*result = NULL;
	controller = opaque;
	resource = private_resource;
	if (resource->owner != private_session ||
	    !resource->blob ||
	    !resource->shareable)
		return EINVAL;
	mutex_lock(&controller->mutex);

	error = bcm2711_shared_create(resource->buffer, image, &shared);

	mutex_unlock(&controller->mutex);

	/* Failed exports retain neither a capability nor a new native reference. */
	if (error != 0)
		return error;
	*result = shared;

	/* Succeeded: the capability survives the original blob and open. */
	return 0;
}

/* Consumes the export's one allocation reference after the common capability retires. */
static void
release_shared(
	void *opaque,
	void *private_shared)
{
	/* Native imports retain their own allocation, rather than this exporting session. */
	(void)opaque;
	bcm2711_shared_release(private_shared);

	/* Succeeded: the capability wrapper has relinquished its allocation reference. */
	return;
}

/* Imports a same-device capability into a separately owned resource descriptor. */
static int
import_resource(
	void *opaque,
	void *private_session,
	void *private_shared,
	uint32_t flags,
	void **result,
	uint32_t *identifier)
{
	struct display_device *controller;
	struct display_session *session;
	struct bcm2711_buffer *buffer;
	struct gpu_image_descriptor image;
	bool has_image;
	int error;

	(void)flags;

	/* Borrowed capability storage remains live throughout this callback. */
	*identifier = 0;
	controller = opaque;
	session = private_session;
	buffer = bcm2711_shared_buffer(private_shared);
	has_image = bcm2711_shared_description(private_shared, &image);
	mutex_lock(&controller->mutex);

	/* The destination identity never aliases a previously retired blob. */
	if (session->next_resource == UINT32_MAX) {
		mutex_unlock(&controller->mutex);
		return ENOSPC;
	}

	/* An allocation-only import owns bytes without inventing a scanout description. */
	if (has_image) {
		error = retain_resource(controller, session, buffer, &image, result);
	} else {
		error = retain_resource(controller, session, buffer, NULL, result);
	}

	/* A failed import owns no receiver descriptor or independent allocation reference. */
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* A fresh renderer-independent identity is required by the common blob wrapper. */
	session->next_resource++;
	*identifier = session->next_resource;

	mutex_unlock(&controller->mutex);

	/* Succeeded: this open owns a new descriptor and an independent native storage hold. */
	return 0;
}

/* Borrows the immutable page vector from the still-live native export capability. */
static int
export_backing(
	void *opaque,
	void *private_shared,
	struct drv_gpu_scanout_backing *backing)
{
	int error;

	/* The capability itself keeps every described address stable. */
	(void)opaque;
	error = bcm2711_shared_backing(private_shared, backing);
	if (error != 0)
		return error;

	/* Succeeded: the receiver can verify the real native physical placement. */
	return 0;
}

/* Imports foreign scanout only after native capability and placement verification. */
static int
import_image(
	void *opaque,
	void *private_session,
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing,
	void **result)
{
	struct display_device *controller;
	struct bcm2711_buffer *buffer;
	int error;

	/* An independent lookup hold protects storage even after the source capability disappears. */
	*result = NULL;
	controller = opaque;
	error = bcm2711_shared_lookup(image, backing, &buffer);
	if (error != 0)
		return error;
	mutex_lock(&controller->mutex);

	error = retain_resource(controller, private_session, buffer, image, result);

	mutex_unlock(&controller->mutex);

	/* The destination owns its own reference on success; both outcomes retire the lookup hold. */
	bcm2711_buffer_release(buffer);
	if (error != 0)
		return error;

	/* Succeeded: common resource teardown owns this complete imported native image. */
	return 0;
}

/* Creates a destination descriptor while the caller serializes resource capacity. */
static int
retain_resource(
	struct display_device *controller,
	struct display_session *session,
	struct bcm2711_buffer *buffer,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct display_resource *resource;
	int error;

	/* The display namespace remains bounded even when the renderer supports larger blobs. */
	(void)controller;
	*result = NULL;
	if (session->resources >= DISPLAY_RESOURCE_COUNT)
		return ENOSPC;
	if (buffer->bytes > DISPLAY_RESOURCE_BYTES)
		return ENOTSUP;
	if (image != NULL) {
		error = bcm2711_shared_image(buffer, image);
		if (error != 0)
			return error;
	}

	/* Allocates the receiver state before retaining the native storage. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;
	resource->owner = session;
	resource->buffer = buffer;
	resource->blob = true;
	resource->shareable = true;
	resource->mappable = true;
	if (image != NULL) {
		resource->image = *image;
		resource->has_image = true;
	}

	/* This reference belongs to the receiver, independently of the exporting capability. */
	bcm2711_buffer_retain(buffer);
	display_hold(buffer, true);
	session->resources++;
	*result = resource;

	/* Succeeded: ordinary resource destruction can retire the independent receiver state. */
	return 0;
}

/* Describes a directly retained blob without copying or changing its immutable image. */
static int
shared_frame(
	struct display_device *controller,
	const struct display_resource *resource,
	const struct gpu_display_present *request,
	struct drv_bcm2711_boot_screen *frame)
{
	uint64_t bytes;

	/* Native direct scanout accepts the exact active mode and one complete linear image. */
	if (!resource->blob || request->flags != (GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB))
		return ENOTSUP;
	if (request->width != controller->native->screen.width ||
	    request->height != controller->native->screen.height ||
	    request->refresh_millihz != controller->native->refresh_millihz)
		return ENOTSUP;
	if (request->format != GPU_PIXEL_RGBA8888 && request->format != GPU_PIXEL_BGRA8888)
		return ENOTSUP;
	if (request->stride < request->width * 4U ||
	    request->stride > 65535U ||
	    (request->stride & 3U) != 0 ||
	    (request->offset & 3U) != 0)
		return EINVAL;
	bytes = (uint64_t)request->stride * request->height;
	if (request->offset > resource->buffer->bytes || bytes > resource->buffer->bytes - request->offset)
		return EINVAL;

	/* Imported images keep the original authoritative layout throughout their lifetime. */
	if (resource->has_image) {
		if (request->width != resource->image.width ||
		    request->height != resource->image.height ||
		    request->format != resource->image.format ||
		    request->stride != resource->image.stride ||
		    request->offset != resource->image.offset)
			return EINVAL;
	}

	/* The physical base and whole allocation were verified before the blob became visible. */
	*frame = controller->native->screen;
	frame->physical = resource->buffer->memory.paddr + request->offset;
	frame->size = bytes;
	frame->pitch = request->stride;
	frame->uncached = resource->buffer->uncached;
	frame->format = 0;
	if (request->format == GPU_PIXEL_RGBA8888)
		frame->format = 1;

	/* Succeeded: an independent controller hold will precede native list publication. */
	return 0;
}


/*
 * Copies a cursor image into the cursor plane's free image (GPU_DISPLAY_CURSOR_SET).
 *
 * A shown cursor shows the new image from the next refresh boundary; a
 * hidden one stays hidden until it is moved.
 */
static int
output_cursor_set(
	void *opaque,
	void *private_session,
	void *private_resource,
	const struct gpu_display_cursor_image *request)
{
	struct display_device *controller;
	struct display_resource *resource;
	struct bcm2711_flip_cursor cursor;
	struct display_cursor_key key;
	struct bcm2711_buffer *target;
	const uint8_t *source;
	unsigned long enabled;
	uint64_t span;
	uint32_t index;
	uint32_t row;
	bool copied;
	int same;
	int error;

	/* Only the lease's own open changes its cursor, from storage it owns. */
	controller = opaque;
	resource = private_resource;
	mutex_lock(&controller->plane_mutex);

	error = cursor_owner(controller, private_session, request->lease, request->generation);
	if (error == 0 && resource->owner != private_session)
		error = ESTALE;
	if (error != 0) {
		mutex_unlock(&controller->plane_mutex);
		return error;
	}

	/* The rows lie inside the buffer the resource names (the core checked its size). */
	span = (uint64_t)request->stride * (request->height - 1U) + (uint64_t)request->width * 4U;
	if (request->offset > resource->buffer->bytes || span > resource->buffer->bytes - request->offset) {
		mutex_unlock(&controller->plane_mutex);
		return EINVAL;
	}

	/* The images are made once, below one GiB for the HVS. */
	for (index = 0; index < DISPLAY_CURSOR_IMAGES; index++) {
		if (controller->cursors[index] != NULL)
			continue;
		error = bcm2711_buffer_create(GPU_DISPLAY_CURSOR_MAX * GPU_DISPLAY_CURSOR_MAX * 4U, 0x3fffffffU, 4096, &controller->cursors[index]);
		if (error != 0) {
			mutex_unlock(&controller->plane_mutex);
			return error;
		}
	}

	/*
	 * The rows' source.  Every writer writes the run first and advances its
	 * generation after, so the generation read before the copy is one the
	 * copied rows are at least as new as.  A run a process maps can change
	 * without a generation step and is never reused.
	 */
	kern_memset(&key, 0, sizeof(key));
	key.identity = resource->buffer->identity;
	key.offset = request->offset;
	key.width = request->width;
	key.height = request->height;
	key.stride = request->stride;
	key.format = request->format;
	enabled = spin_lock_irqsave(&resource->buffer->guard);

	key.generation = resource->buffer->generation;

	spin_unlock_irqrestore(&resource->buffer->guard, enabled);

	/* Reuse is possible only for an identified run no process maps. */
	if (key.identity != 0 && !resource->buffer->host_mapped)
		key.valid = true;

	/* An image that holds the same unchanged rows is shown again without a copy, even while a list names it. */
	target = NULL;
	for (index = 0; index < DISPLAY_CURSOR_IMAGES && key.valid && target == NULL; index++) {
		same = kern_memcmp(&controller->keys[index], &key, sizeof(key));
		if (same == 0)
			target = controller->cursors[index];
	}

	/* Otherwise an image no list or recorded cursor names takes the rows, packed at the plane's pitch, without waiting. */
	for (index = 0; index < DISPLAY_CURSOR_IMAGES && target == NULL; index++) {
		if (!bcm2711_display_flip_cursor_named(controller->native, (uint32_t)controller->cursors[index]->memory.paddr))
			break;
	}
	if (target == NULL && index == DISPLAY_CURSOR_IMAGES) {
		mutex_unlock(&controller->plane_mutex);
		return EBUSY;
	}
	copied = false;
	if (target == NULL) {
		target = controller->cursors[index];
		copied = true;

		/* A cacheable source may hold lines older than the device's writes; they go before the rows are read. */
		if (!resource->buffer->uncached)
			kern_dcache_invalidate_range((const uint8_t *)resource->buffer->address + request->offset, (size_t)span);

		/* The rows, then the image's new key whatever the flip below reports. */
		for (row = 0; row < request->height; row++) {
			source = (const uint8_t *)resource->buffer->address + request->offset + (uint64_t)row * request->stride;
			kern_memcpy((uint8_t *)target->address + (uint64_t)row * GPU_DISPLAY_CURSOR_MAX * 4U, source, request->width * 4U);
		}
		kern_dcache_clean_range(target->address, (size_t)GPU_DISPLAY_CURSOR_MAX * 4U * request->height);
		kern_io_write_barrier();
		kern_memcpy(&controller->keys[index], &key, sizeof(key));
	}

	/* The flip report counts the set, and whether it copied rows. */
	bcm2711_display_flip_count_cursor(controller->native, copied);

	/* The plane names the new image from now on, shown or not. */
	cursor = controller->cursor;
	cursor.physical = (uint32_t)target->memory.paddr;
	cursor.width = request->width;
	cursor.height = request->height;
	cursor.pitch = GPU_DISPLAY_CURSOR_MAX * 4U;
	cursor.format = 0;
	if (request->format == GPU_PIXEL_RGBA8888)
		cursor.format = 1;
	error = cursor_record(controller, private_session, request->lease, &cursor);

	mutex_unlock(&controller->plane_mutex);

	/* The cadence notes the set's time outside the mutex, as its report may log. */
	bcm2711_display_flip_note(controller->native, BCM2711_FLIP_CADENCE_SET);

	/* A refused record leaves the previous image whole while a list may name it. */
	if (error != 0)
		return error;

	/* Succeeded: the new image shows from a coming refresh boundary when the cursor is shown. */
	return 0;
}

/* Shows the cursor with its top-left corner at a point (GPU_DISPLAY_CURSOR_MOVE). */
static int
output_cursor_move(
	void *opaque,
	void *private_session,
	const struct gpu_display_cursor_move *request)
{
	struct display_device *controller;
	struct bcm2711_flip_cursor cursor;
	int error;

	/* Only the lease's own open moves its cursor, once it has an image. */
	controller = opaque;
	mutex_lock(&controller->plane_mutex);

	error = cursor_owner(controller, private_session, request->lease, request->generation);
	if (error == 0 && controller->cursor.physical == 0)
		error = EINVAL;
	if (error != 0) {
		mutex_unlock(&controller->plane_mutex);
		return error;
	}

	/* The flip state publishes it without waiting. */
	cursor = controller->cursor;
	cursor.shown = true;
	cursor.x = request->x;
	cursor.y = request->y;
	error = cursor_record(controller, private_session, request->lease, &cursor);

	mutex_unlock(&controller->plane_mutex);

	/* The cadence notes the move's time outside the mutex, as its report may log. */
	bcm2711_display_flip_note(controller->native, BCM2711_FLIP_CADENCE_MOVE);

	/* Reports a refused record. */
	if (error != 0)
		return error;

	/* Succeeded: the cursor shows there from a coming refresh boundary. */
	return 0;
}

/* Hides the cursor, keeping its image (GPU_DISPLAY_CURSOR_HIDE). */
static int
output_cursor_hide(
	void *opaque,
	void *private_session,
	const struct gpu_display_cursor_hide *request)
{
	struct display_device *controller;
	struct bcm2711_flip_cursor cursor;
	int error;

	/* Only the lease's own open hides its cursor. */
	controller = opaque;
	mutex_lock(&controller->plane_mutex);

	error = cursor_owner(controller, private_session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&controller->plane_mutex);
		return error;
	}

	/* The flip state publishes a list without it. */
	cursor = controller->cursor;
	cursor.shown = false;
	error = cursor_record(controller, private_session, request->lease, &cursor);

	mutex_unlock(&controller->plane_mutex);

	/* Reports a refused record. */
	if (error != 0)
		return error;

	/* Succeeded: the cursor is gone from a coming refresh boundary. */
	return 0;
}

/* Checks that an open holds the output's lease of the current generation, against the flip state's copy. */
static int
cursor_owner(
	struct display_device *controller,
	struct display_session *session,
	uint64_t lease,
	uint64_t generation)
{
	struct bcm2711_flip_owner owner;
	int error;

	/* Another open's lease, an ending one, or none, cannot change the cursor. */
	lease_owner(session, lease, &owner);
	error = bcm2711_display_flip_owned(controller->native, &owner);
	if (error != 0)
		return EBUSY;
	if (generation != 1)
		return ESTALE;

	/* Succeeded: the open owns the output. */
	return 0;
}

/* Records the cursor in the flip state for an open's lease; the caller holds the plane mutex. */
static int
cursor_record(
	struct display_device *controller,
	struct display_session *session,
	uint64_t lease,
	const struct bcm2711_flip_cursor *cursor)
{
	struct bcm2711_flip_owner owner;
	int error;

	/* The flip state checks the lease again where it records. */
	lease_owner(session, lease, &owner);
	error = bcm2711_display_flip_cursor(controller->native, &owner, cursor);
	if (error != 0)
		return error;

	/* The plane's own copy follows the recorded cursor. */
	controller->cursor = *cursor;

	/* Succeeded: the cursor shows from a coming refresh boundary. */
	return 0;
}

/*
 * Forgets the cursor's image when a lease begins or ends; the caller holds the controller mutex.
 *
 * The plane mutex waits out a cursor change in progress.  The flip state
 * hides its own cursor when the console comes back and when a lease begins.
 */
static void
cursor_reset(
	struct display_device *controller)
{
	/* A hidden, imageless cursor, and the next lease's images copied afresh. */
	mutex_lock(&controller->plane_mutex);

	kern_memset(&controller->cursor, 0, sizeof(controller->cursor));
	kern_memset(controller->keys, 0, sizeof(controller->keys));

	mutex_unlock(&controller->plane_mutex);
}

/* Counts one display resource more or less on a run; the render executor favours executions that use a displayed run. */
static void
display_hold(
	struct bcm2711_buffer *buffer,
	bool hold)
{
	unsigned long enabled;

	/* The run's guard also protects its references. */
	enabled = spin_lock_irqsave(&buffer->guard);

	if (hold) {
		if (buffer->displayed == UINT32_MAX)
			__builtin_trap();
		buffer->displayed++;
	} else {
		if (buffer->displayed == 0)
			__builtin_trap();
		buffer->displayed--;
	}

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Succeeded: the count follows the display resources. */
	return;
}
