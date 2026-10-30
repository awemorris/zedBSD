/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display node of the Mali driver: the screen the bootloader lit.
 *
 * The bootloader brings up the HDMI output and leaves the VPU's first
 * OSD plane scanning out its framebuffer, which the kernel's text
 * console draws on.  This node gives that screen to a session through
 * the display operations (query, mode, claim, release, present, wait):
 * one output, one plane, one mode -- the bootloader's extent, reported at
 * 60 Hz -- and one lease at a time.  It never changes the output, its
 * mode, its clocks or the plane's window and format; the only register
 * it writes is the plane's buffer address.
 *
 * A resource is a run of this node's own memory, or an image the render
 * node exported and the session imported for shared frames: a linear
 * image of a packed four-byte format in one contiguous run of RAM, which
 * the resource holds through the render node until it is freed.  An
 * image of another shape is not imported, and the swapchain copies its
 * frames into a run of this node instead.
 *
 * A presentation is shown one of two ways.  When the plane reads a
 * linear buffer and shows exactly the boot framebuffer, a frame of the
 * screen's extent, row pitch and byte order, at a 16-byte aligned place
 * of a resource (contiguous, below 4 GiB), is shown by pointing the
 * plane at the resource itself: a run's frame is first cleaned out of
 * the processor's caches (an import is written by the GPU and read by
 * the processor only through an uncached view), the address is written,
 * and the presentation returns one frame later, when the plane reads the
 * new buffer and the previous one is free again.  Any other frame is
 * copied by the processor onto the framebuffer, row by row, clipped to
 * the screen, with red and blue swapped when the orders differ and the
 * alpha byte set (the plane blends by it), and the plane is pointed back
 * at the framebuffer if it showed a resource.
 *
 * A resource the plane shows outlives its destruction by the session:
 * it is freed once the plane shows something else.  While a lease is
 * held the text console does not draw; the screen is cleared for the
 * lease's frames and given back to the console, with the plane on the
 * framebuffer, when the lease is released or its holder closes.
 *
 * Procedure referenced:
 * src/drivers/gpu/powervr/display/display.c (the display operations'
 * contract on a boot framebuffer).
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <drivers/gpu/gpu-scanout.h>
#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#include "drivers/gpu/mali/display/osd.h"
#include "drivers/gpu/mali/display/scanout.h"
#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/render.h"

/* The one output's identifier and generation; the screen never changes. */
#define DISPLAY_ID			1U
#define DISPLAY_GENERATION		1U

/* The refresh reported for the bootloader's screen, which does not tell its own. */
#define DISPLAY_REFRESH			60000U

/* How long a frame lasts at the slowest refresh the screen may have (20 ms). */
#define DISPLAY_FRAME_MS		20U

/* The resources one session may hold, and the largest one (64 MiB). */
#define DISPLAY_RESOURCES		16U
#define DISPLAY_RESOURCE_BYTES		(64ULL << 20)

/* The page resources are allocated in, and the bytes of one pixel. */
#define DISPLAY_PAGE_BYTES		4096U
#define DISPLAY_PIXEL_BYTES		4U

/* What the plane needs of a buffer it reads: rows and start on 16 bytes, below 4 GiB. */
#define DISPLAY_SCAN_ALIGNMENT		16U
#define DISPLAY_SCAN_LIMIT		0xffffffffULL

/* The alpha byte every copied pixel gets, since the plane blends by it. */
#define DISPLAY_OPAQUE			0xff000000U

/* The name a query reports, and the driver name of the node. */
#define DISPLAY_NAME			"mali osd1"
#define DISPLAY_DRIVER			"mali-display"

/* The capabilities of the node: its own storage, copies, CPU views and the display. */
#define DISPLAY_CAPABILITIES		(GPU_CAP_RESOURCE | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_DISPLAY)

/*
 * One open of the node.
 *
 * Made by open and freed by close, after the core has destroyed every
 * resource of the open; the count is kept under the node's mutex.
 */
struct display_session {
	uint32_t resources;
};

/*
 * One resource of a session: a contiguous run of ordinary cacheable RAM
 * below 4 GiB, or an imported image of the render node.
 *
 * Made by resource_create (a run) or import_image (an import, whose
 * render node object bo is held) and freed by resource_destroy, or, when
 * the plane shows it at that time, by whatever moves the plane off it
 * (destroyed is then set, under the node's mutex, and the owner is never
 * looked at again).  physical and address are the first byte of the
 * memory (the run, or the exported object) as the plane and the
 * processor see it; an import's address is the render node's uncached
 * view, and image is the layout it was exported with.  None of them
 * change in between.
 */
struct display_resource {
	struct display_session *owner;
	struct kern_pmem run;
	struct mali_bo *bo;
	struct gpu_image_descriptor image;
	uint64_t physical;
	uint8_t *address;
	uint64_t bytes;
	int destroyed;
};

/*
 * The node: the screen, the plane, and the lease.
 *
 * Filled once by mali_display_attach() on the platform's thread;
 * afterwards the lease fields and front change only under the mutex,
 * and the rest does not change.
 */
struct display {
	/* Nonzero once the node is registered. */
	int ready;

	/* The bootloader's screen as the kernel reads it, and whether its bytes are R, G, B, X. */
	struct kern_boot_framebuffer screen;
	int rgbx;

	/* The processor's view of the screen; direct is nonzero for the direct map, cleaned after writes. */
	uint8_t *pixels;
	size_t view_bytes;
	int direct;

	/* The VPU's window (NULL when unmapped), the plane as found, and whether frames may be flipped. */
	volatile uint8_t *vpu;
	struct mali_osd_plane plane;
	int flip;
	uint32_t framebuffer_word;

	/* Protects the fields below. */
	struct mutex mutex;

	/* The session holding the lease (NULL when none), its lease, and the next lease's number. */
	struct display_session *owner;
	uint64_t lease;
	uint64_t next_lease;

	/* Frames shown under the lease, and the tick the newest was shown at. */
	uint64_t sequence;
	uint64_t present_tick;

	/* The resource the plane shows; NULL while it shows the framebuffer. */
	struct display_resource *front;

	/* The route the log last named under this lease (-1 before the first frame). */
	int logged_route;

	/* Nonzero once a refused import was said; later ones are not. */
	int import_logged;

	/* The core's handle of the registered node. */
	struct drv_gpu_device *gpu;
};

/* Declared before the operation tables that name them. */
static int display_open(void *device, void **result);
static void display_close(void *device, void *session);
static int display_info(void *device, void *session, struct gpu_info *info);
static int resource_create(void *device, void *session, const struct gpu_resource_create *request, void **result);
static void resource_destroy(void *device, void *session, void *resource);
static int resource_read(void *device, void *session, void *resource, uint64_t offset, void *data, uint32_t bytes);
static int resource_write(void *device, void *session, void *resource, uint64_t offset, const void *data, uint32_t bytes);
static int resource_map(void *device, void *session, void *resource, struct drv_gpu_mapping *mapping);
static int output_query(void *device, void *session, struct gpu_display_info *request);
static int output_mode(void *device, void *session, struct gpu_display_mode *request);
static int output_claim(void *device, void *session, struct gpu_display_claim *request);
static int output_release(void *device, void *session, const struct gpu_display_release *request);
static int output_present(void *device, void *session, void *resource, struct gpu_display_present *request);
static int output_wait(void *device, void *session, struct gpu_display_wait *request);
static int output_device(void *device, void *session, struct gpu_device_info *info);
static int output_constraints(void *device, void *session, struct gpu_scanout_constraints *request);
static int import_image(void *device, void *session, const struct gpu_image_descriptor *image, const struct drv_gpu_scanout_backing *backing, void **result);

/* The display operations of the node. */
static const struct drv_gpu_display_ops display_operations = {
	.query = output_query,
	.mode = output_mode,
	.claim = output_claim,
	.release = output_release,
	.present = output_present,
	.wait = output_wait,
};

/* The scanout operations: the node's role and constraints, and the import of the render node's images. */
static const struct drv_gpu_scanout_ops scanout_operations = {
	.query_device = output_device,
	.constraints = output_constraints,
	.import_image = import_image,
};

/* The operations of the node. */
static const struct drv_gpu_ops node_operations = {
	.version = DRV_GPU_INTERFACE_VERSION,
	.size = sizeof(struct drv_gpu_ops),
	.capabilities = DISPLAY_CAPABILITIES,
	.open = display_open,
	.close = display_close,
	.get_info = display_info,
	.resource_create = resource_create,
	.resource_destroy = resource_destroy,
	.resource_read = resource_read,
	.resource_write = resource_write,
	.resource_map = resource_map,
	.display = &display_operations,
	.scanout = &scanout_operations,
};

/*
 * The board's one display node, set up by mali_display_attach().
 */
static struct display board_display;

static int screen_valid(const struct kern_boot_framebuffer *screen);
static int map_screen(struct display *display);
static void find_plane(struct display *display, const struct drv_fdt *fdt);
static void clear_screen(struct display *display);
static void copy_frame(struct display *display, const struct display_resource *resource, const struct gpu_display_present *request);
static int frame_looks_opaque(const struct display_resource *resource, const struct gpu_display_present *request);
static int can_flip(const struct display *display, const struct display_resource *resource, const struct gpu_display_present *request);
static void point_plane(struct display *display, uint64_t address, struct display_resource *front, struct display_resource **freed);
static void end_lease(struct display *display, struct display_resource **freed);
static void free_resource(struct display_resource *resource);
static int shared_frame_valid(const struct display_resource *resource, const struct gpu_display_present *request);

/*
 * Registers the display node on the bootloader's screen, and finds out
 * whether the VPU's first plane shows it so frames can be flipped.
 */
int
mali_display_attach(
	const struct drv_fdt *fdt)
{
	struct display *display;
	int valid;
	int error;

	/* Refuses a second node. */
	display = &board_display;
	if (display->ready)
		return EALREADY;

	/* Reads the bootloader's screen; a boot without one has no display node. */
	error = kern_boot_framebuffer("odroidc5.framebuffer", &display->screen);
	if (error != 0) {
		kern_logf("mali: no display: no boot framebuffer (%d)\n", error);
		return error;
	}

	/* Refuses a screen the copies cannot write. */
	valid = screen_valid(&display->screen);
	if (!valid) {
		kern_logf("mali: no display: framebuffer %ux%u pitch %u format %u not usable\n",
			  display->screen.width,
			  display->screen.height,
			  display->screen.pitch,
			  display->screen.format);
		return ENODEV;
	}

	/* The board's console names red first as 1; blue first as 0 or as the boot code's own value. */
	display->rgbx = 0;
	if (display->screen.format == KERN_BOOT_FRAMEBUFFER_RGBX8888)
		display->rgbx = 1;

	/* The processor's view of the screen. */
	error = map_screen(display);
	if (error != 0) {
		kern_logf("mali: no display: framebuffer map failed (%d)\n", error);
		return error;
	}

	/* The plane, read only, to decide whether frames may be flipped. */
	find_plane(display, fdt);

	/* The lease's lock; the lease starts free and lease numbers start at one. */
	error = mutex_init(&display->mutex, LOCK_RANK_DEVICE, "mali-display");
	if (error != 0) {
		kern_logf("mali: no display: lock failed (%d)\n", error);
		return error;
	}

	display->owner = NULL;
	display->lease = 0U;
	display->next_lease = 1U;
	display->front = NULL;

	/* Publishes the node; it is the first GPU device of the board. */
	error = drv_gpu_register(&node_operations, display, &display->gpu);
	if (error != 0) {
		kern_logf("mali: no display: register failed (%d)\n", error);
		return error;
	}

	display->ready = 1;
	kern_logf("mali: display %ux%u pitch %u on the boot framebuffer at %llx, frames %s\n",
		  display->screen.width,
		  display->screen.height,
		  display->screen.pitch,
		  (unsigned long long)display->screen.physical_base,
		  display->flip ? "flipped" : "copied");

	/* Succeeded: the screen is a display node. */
	return 0;
}

/* Creates a session without a resource or the lease. */
static int
display_open(
	void *device,
	void **result)
{
	struct display_session *session;

	(void)device;

	/* Each open starts with nothing. */
	*result = NULL;
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* Succeeded: the core owns the session. */
	*result = session;
	return 0;
}

/* Ends a session; its lease, if it still holds one, gives the screen back to the console. */
static void
display_close(
	void *device,
	void *private_session)
{
	struct display *display;
	struct display_session *session;
	struct display_resource *freed;

	display = device;
	session = private_session;

	/* A holder that did not release its lease loses it here. */
	freed = NULL;
	mutex_lock(&display->mutex);

	if (display->owner == session)
		end_lease(display, &freed);

	mutex_unlock(&display->mutex);

	/* The destroyed resource the plane let go of. */
	if (freed != NULL)
		free_resource(freed);

	kern_free(session);
}

/* Reports the node's capabilities and storage limits. */
static int
display_info(
	void *device,
	void *session,
	struct gpu_info *info)
{
	(void)device;
	(void)session;

	/* What the node does and how much storage a session may hold. */
	info->capabilities = DISPLAY_CAPABILITIES;
	info->max_resources = DISPLAY_RESOURCES;
	info->max_resource_bytes = DISPLAY_RESOURCE_BYTES;
	kern_snprintf(info->driver_name, sizeof(info->driver_name), "%s", DISPLAY_DRIVER);

	/* Succeeded: the limits describe the node's own storage. */
	return 0;
}

/*
 * Allocates one storage resource of a session: contiguous cacheable RAM
 * below 4 GiB, so the plane can read it directly.
 */
static int
resource_create(
	void *device,
	void *private_session,
	const struct gpu_resource_create *request,
	void **result)
{
	struct display *display;
	struct display_session *session;
	struct display_resource *resource;
	uint64_t bytes;
	int error;

	display = device;
	session = private_session;
	*result = NULL;

	/* Only plain storage of a bounded size. */
	if (request->bytes == 0U || request->bytes > DISPLAY_RESOURCE_BYTES)
		return EINVAL;
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE || request->flags != 0U)
		return ENOTSUP;

	/* Whole pages, so a CPU view of the run maps nothing else. */
	bytes = (request->bytes + DISPLAY_PAGE_BYTES - 1U) & ~(uint64_t)(DISPLAY_PAGE_BYTES - 1U);

	/* The record first, then its memory. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	error = kern_pmem_alloc_limited((size_t)bytes, DISPLAY_PAGE_BYTES, DISPLAY_SCAN_LIMIT, 0U, &resource->run);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	resource->address = kern_pmem_to_kernel(resource->run.paddr);
	if (resource->address == NULL) {
		(void)kern_pmem_free(&resource->run);
		kern_free(resource);
		return ENOMEM;
	}

	/* New storage holds zeroes, not an earlier owner's bytes. */
	kern_memset(resource->address, 0, (size_t)bytes);
	resource->owner = session;
	resource->physical = resource->run.paddr;
	resource->bytes = request->bytes;

	/* Counts the resource against the session's limit. */
	mutex_lock(&display->mutex);

	if (session->resources >= DISPLAY_RESOURCES) {
		mutex_unlock(&display->mutex);
		(void)kern_pmem_free(&resource->run);
		kern_free(resource);
		return ENOSPC;
	}

	session->resources++;

	mutex_unlock(&display->mutex);

	/* Succeeded: the core owns the resource until resource_destroy. */
	*result = resource;
	return 0;
}

/* Frees one resource after the core has dropped every view and pin of it, or marks it when the plane shows it. */
static void
resource_destroy(
	void *device,
	void *private_session,
	void *private_resource)
{
	struct display *display;
	struct display_session *session;
	struct display_resource *resource;
	int shown;

	display = device;
	session = private_session;
	resource = private_resource;

	/* Gives the place back, and leaves a shown resource to whatever moves the plane off it. */
	mutex_lock(&display->mutex);

	if (session->resources != 0U)
		session->resources--;
	shown = 0;
	if (display->front == resource) {
		resource->destroyed = 1;
		resource->owner = NULL;
		shown = 1;
	}

	mutex_unlock(&display->mutex);

	/* A resource the plane does not read is freed now. */
	if (!shown)
		free_resource(resource);
}

/* Copies bytes out of a resource. */
static int
resource_read(
	void *device,
	void *session,
	void *private_resource,
	uint64_t offset,
	void *data,
	uint32_t bytes)
{
	struct display_resource *resource;

	(void)device;
	resource = private_resource;

	/* Only the owner's run, and only inside it; an import is the render node's to read. */
	if (resource->owner != session)
		return EINVAL;
	if (resource->bo != NULL)
		return ENOTSUP;
	if (offset > resource->bytes || bytes > resource->bytes - offset)
		return EINVAL;

	/* The run is ordinary RAM the CPU reads through the direct map. */
	kern_memcpy(data, resource->address + offset, bytes);

	/* Succeeded: the bytes are copied. */
	return 0;
}

/* Copies bytes into a resource. */
static int
resource_write(
	void *device,
	void *session,
	void *private_resource,
	uint64_t offset,
	const void *data,
	uint32_t bytes)
{
	struct display_resource *resource;

	(void)device;
	resource = private_resource;

	/* Only the owner's run, and only inside it; an import is the render node's to write. */
	if (resource->owner != session)
		return EINVAL;
	if (resource->bo != NULL)
		return ENOTSUP;
	if (offset > resource->bytes || bytes > resource->bytes - offset)
		return EINVAL;

	/* The run is ordinary RAM the CPU writes through the direct map. */
	kern_memcpy(resource->address + offset, data, bytes);

	/* Succeeded: the bytes are copied. */
	return 0;
}

/* Describes a resource's run for a cacheable CPU view in the session's address space. */
static int
resource_map(
	void *device,
	void *session,
	void *private_resource,
	struct drv_gpu_mapping *mapping)
{
	struct display_resource *resource;

	(void)device;
	resource = private_resource;

	/* Only the owner maps its run; an import is mapped through the render node. */
	if (resource->owner != session)
		return EINVAL;
	if (resource->bo != NULL)
		return ENOTSUP;

	/* Ordinary cacheable RAM; a flip cleans the frame out of the caches before the plane reads it. */
	mapping->physical = resource->run.paddr;
	mapping->address = resource->address;
	mapping->bytes = resource->run.size;
	mapping->attributes = 0U;

	/* Succeeded: the view stays valid while the resource lives. */
	return 0;
}

/* Describes the one output: the bootloader's screen. */
static int
output_query(
	void *device,
	void *session,
	struct gpu_display_info *request)
{
	struct display *display;

	(void)session;
	display = device;

	/* One output. */
	request->count = 1U;
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;
	if (request->index != 0U)
		return EINVAL;

	/* Connected, FIFO, active while a lease has shown a frame. */
	request->display_id = DISPLAY_ID;
	request->generation = DISPLAY_GENERATION;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO;
	mutex_lock(&display->mutex);

	if (display->owner != NULL && display->sequence != 0U)
		request->flags |= GPU_DISPLAY_ACTIVE;

	mutex_unlock(&display->mutex);

	/* One plane, the two packed formats, and the largest frame taken. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = DISPLAY_RESOURCE_BYTES;

	/* The bootloader's extent is the current, preferred and largest one. */
	request->current_width = display->screen.width;
	request->current_height = display->screen.height;
	request->preferred_width = display->screen.width;
	request->preferred_height = display->screen.height;
	request->max_width = display->screen.width;
	request->max_height = display->screen.height;
	request->refresh_millihz = DISPLAY_REFRESH;

	/* The bootloader does not tell the screen's physical size. */
	request->physical_width_mm = 0U;
	request->physical_height_mm = 0U;
	kern_snprintf(request->name, sizeof(request->name), "%s", DISPLAY_NAME);

	/* Succeeded: the output is described. */
	return 0;
}

/* Enumerates the one mode, or checks a mode: the bootloader's extent or a smaller one, at 60 Hz. */
static int
output_mode(
	void *device,
	void *session,
	struct gpu_display_mode *request)
{
	struct display *display;

	(void)session;
	display = device;

	/* Only the one output of this generation. */
	if (request->display_id != DISPLAY_ID)
		return ENOENT;
	if (request->generation != DISPLAY_GENERATION)
		return ESTALE;

	/* Enumeration: the bootloader's mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;

		request->width = display->screen.width;
		request->height = display->screen.height;
		request->refresh_millihz = DISPLAY_REFRESH;
		return 0;
	}

	/* Validation: an extent that fits, at the one refresh (zero asks for it). */
	if (request->operation != GPU_DISPLAY_MODE_VALIDATE)
		return EINVAL;
	if (request->width == 0U || request->height == 0U)
		return EINVAL;
	if (request->width > display->screen.width || request->height > display->screen.height)
		return EINVAL;
	if (request->refresh_millihz != 0U && request->refresh_millihz != DISPLAY_REFRESH)
		return EINVAL;

	request->refresh_millihz = DISPLAY_REFRESH;

	/* Succeeded: the mode is shown unscaled at the top left. */
	return 0;
}

/*
 * Gives the lease to a session: the text console stops drawing and the
 * screen is cleared for the lease's frames.  EBUSY while another session
 * holds it.
 */
static int
output_claim(
	void *device,
	void *private_session,
	struct gpu_display_claim *request)
{
	struct display *display;
	struct display_session *session;

	display = device;
	session = private_session;

	/* Only plane 0 of the one output of this generation. */
	if (request->display_id != DISPLAY_ID)
		return ENOENT;
	if (request->generation != DISPLAY_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

	/* Takes the lease when it is free. */
	mutex_lock(&display->mutex);

	if (display->owner != NULL) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}

	display->owner = session;
	display->lease = display->next_lease;
	display->next_lease++;
	display->sequence = 0U;
	display->logged_route = -1;
	request->lease = display->lease;

	/* The console stops drawing before the screen is cleared for the frames. */
	kern_text_suspend();
	clear_screen(display);

	mutex_unlock(&display->mutex);

	/* Succeeded: the session holds the screen. */
	return 0;
}

/* Ends the lease of its holder and gives the screen back to the text console. */
static int
output_release(
	void *device,
	void *session,
	const struct gpu_display_release *request)
{
	struct display *display;
	struct display_resource *freed;

	display = device;

	/* Only the holder releases its own lease. */
	freed = NULL;
	mutex_lock(&display->mutex);

	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EINVAL;
	}

	end_lease(display, &freed);

	mutex_unlock(&display->mutex);

	/* The destroyed resource the plane let go of. */
	if (freed != NULL)
		free_resource(freed);

	/* Succeeded: the console draws the screen again. */
	return 0;
}

/*
 * Shows a frame of the lease holder; it is on the screen when this
 * returns.  The plane is pointed at the resource when it can read it
 * directly; otherwise the processor copies the frame onto the
 * framebuffer and the plane goes back to the framebuffer.
 */
static int
output_present(
	void *device,
	void *session,
	void *private_resource,
	struct gpu_display_present *request)
{
	struct display *display;
	struct display_resource *resource;
	struct display_resource *freed;
	uint64_t frame_bytes;
	int direct;
	int shared;
	int route;
	int error;

	display = device;
	resource = private_resource;
	freed = NULL;

	/*
	 * Only the session's own resource; the core checked the frame lies
	 * inside it.  A shared frame is an import in its exported layout, an
	 * ordinary one a frame of a run.
	 */
	if (resource->owner != session)
		return EINVAL;
	shared = (request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U;
	if (shared) {
		error = shared_frame_valid(resource, request);
		if (error != 0)
			return error;
	} else if (resource->bo != NULL) {
		return EINVAL;
	}

	/* Shows the frame under the mutex, for the holder of this lease and generation only. */
	mutex_lock(&display->mutex);

	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}

	if (request->generation != DISPLAY_GENERATION) {
		mutex_unlock(&display->mutex);
		return ESTALE;
	}

	/* The plane reads the resource itself when it can; else the frame is copied and the plane goes back. */
	direct = can_flip(display, resource, request);
	if (direct) {
		if (resource->bo == NULL) {
			frame_bytes = (uint64_t)request->stride * request->height;
			kern_dcache_clean_range(resource->address + request->offset, (size_t)frame_bytes);
		}

		point_plane(display, resource->physical + request->offset, resource, &freed);
	} else {
		copy_frame(display, resource, request);
		if (display->front != NULL)
			point_plane(display, display->screen.physical_base, NULL, &freed);
	}

	/* Says the route when it changes: flipped or copied, from an import or a run. */
	route = direct;
	if (shared)
		route |= 2;
	if (route != display->logged_route) {
		display->logged_route = route;
		kern_logf("mali: display lease %llu: %s frames %s\n",
			  (unsigned long long)display->lease,
			  shared ? "shared" : "copied-in",
			  direct ? "flipped" : "copied");
	}

	/* Counts the frame shown. */
	display->sequence++;
	display->present_tick = sched_ticks();
	request->sequence = display->sequence;

	mutex_unlock(&display->mutex);

	/* The destroyed resource the plane let go of. */
	if (freed != NULL)
		free_resource(freed);

	/* Succeeded: the frame is on the screen. */
	return 0;
}

/*
 * Reports the newest frame shown under the holder's lease.  A frame is on
 * the screen when its presentation returns, so this never waits.
 */
static int
output_wait(
	void *device,
	void *session,
	struct gpu_display_wait *request)
{
	struct display *display;

	display = device;

	/* Reads the lease's count under the mutex. */
	mutex_lock(&display->mutex);

	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}

	if (request->sequence > display->sequence) {
		mutex_unlock(&display->mutex);
		return EINVAL;
	}

	if (display->sequence == 0U) {
		mutex_unlock(&display->mutex);
		return EAGAIN;
	}

	request->completed_sequence = display->sequence;
	request->present_time_ns = kern_ticks_to_ns(display->present_tick);
	request->generation = DISPLAY_GENERATION;

	mutex_unlock(&display->mutex);

	/* Succeeded: every frame up to the sequence is on the screen. */
	return 0;
}

/* Describes the node as a display-only device. */
static int
output_device(
	void *device,
	void *session,
	struct gpu_device_info *info)
{
	(void)device;
	(void)session;

	/* The core fills the device's identifier. */
	info->roles = GPU_DEVICE_DISPLAY;
	info->companion_id = 0U;
	info->flags = 0U;

	/* Succeeded: the node is a display. */
	return 0;
}

/* Reports the one route the node takes frames by: from its own storage. */
static int
output_constraints(
	void *device,
	void *session,
	struct gpu_scanout_constraints *request)
{
	(void)device;
	(void)session;

	/* Only the one output of this generation. */
	if (request->display_id != DISPLAY_ID)
		return ENXIO;
	if (request->generation != DISPLAY_GENERATION)
		return ESTALE;

	/*
	 * Frames from the node's own storage, and images the render node
	 * exported (a trial: an image that is not one contiguous linear run
	 * is refused and the swapchain copies instead), in the two packed
	 * formats, rows of whole pixels.  An import below 4 GiB may be
	 * flipped; one above is copied, so no placement is asked for.
	 */
	request->flags = GPU_SCANOUT_COPY | GPU_SCANOUT_SHARED | GPU_SCANOUT_FOREIGN;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->stride_alignment = DISPLAY_PIXEL_BYTES;
	request->offset_alignment = DISPLAY_PIXEL_BYTES;
	request->placement = 0U;
	request->max_dma_address = 0U;

	/* Succeeded: the constraints of the node's route. */
	return 0;
}

/*
 * Imports an image the render node exported, for shared frames: a
 * linear image of a packed four-byte format in one contiguous run of
 * RAM, whose object the resource holds through the render node.  Any
 * other image, or a backing the render node did not hand out, is
 * ENOTSUP (said once), and the swapchain copies its frames instead.
 */
static int
import_image(
	void *device,
	void *private_session,
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing,
	void **result)
{
	struct display *display;
	struct display_session *session;
	struct display_resource *resource;
	struct mali_bo *bo;
	const char *reason;
	uint64_t physical;
	int error;

	display = device;
	session = private_session;

	/* Owns nothing on failure. */
	*result = NULL;

	/* An image the plane or the copies can read from one physical run. */
	reason = NULL;
	error = mali_scanout_check(image, backing, &physical, &reason);
	if (error == 0) {
		/* An export of the render node, held while the resource lives. */
		error = mali_render_scanout_hold(backing, &bo);
		if (error != 0)
			reason = "is not an export of the render node";
	}

	if (error != 0) {
		if (!display->import_logged) {
			display->import_logged = 1;
			kern_logf("mali: display: an image is not imported (%d): the image %s; its frames are copied\n", error, reason);
		}

		return error;
	}

	/* The record of the import. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL) {
		mali_render_scanout_drop(bo);
		return ENOMEM;
	}

	resource->owner = session;
	resource->bo = bo;
	resource->image = *image;
	resource->physical = physical - image->offset;
	resource->address = (uint8_t *)(uintptr_t)bo->buffer.view;
	resource->bytes = backing->bytes;

	/* Counts the import against the session's limit. */
	mutex_lock(&display->mutex);

	if (session->resources >= DISPLAY_RESOURCES) {
		mutex_unlock(&display->mutex);
		mali_render_scanout_drop(bo);
		kern_free(resource);
		return ENOSPC;
	}

	session->resources++;

	mutex_unlock(&display->mutex);

	/* Succeeded: the resource holds the object until resource_destroy. */
	*result = resource;
	return 0;
}

/* Reports whether the copies can write a screen: a known format and room for every row. */
static int
screen_valid(
	const struct kern_boot_framebuffer *screen)
{
	uint64_t needed;

	/* A visible extent, and rows of whole pixels at least as long as the width. */
	if (screen->width == 0U || screen->height == 0U)
		return 0;
	if ((screen->pitch % DISPLAY_PIXEL_BYTES) != 0U)
		return 0;
	if ((uint64_t)screen->pitch < (uint64_t)screen->width * DISPLAY_PIXEL_BYTES)
		return 0;

	/* Memory for every row. */
	needed = (uint64_t)screen->pitch * screen->height;
	if (screen->size < needed)
		return 0;

	/* Succeeded: the screen can be written. */
	return 1;
}

/*
 * Makes the processor's view of the screen: the direct map for a screen
 * in RAM, written as the text console writes it and cleaned after, or a
 * device view for one outside RAM.
 */
static int
map_screen(
	struct display *display)
{
	uint64_t offset;
	void *view;
	int error;

	/* The whole pages the screen lies in. */
	offset = display->screen.physical_base & (DISPLAY_PAGE_BYTES - 1U);
	display->view_bytes = (size_t)((offset + display->screen.size + DISPLAY_PAGE_BYTES - 1U) & ~(uint64_t)(DISPLAY_PAGE_BYTES - 1U));

	/* A screen in RAM: the direct map. */
	display->pixels = kern_pmem_to_kernel(display->screen.physical_base);
	if (display->pixels != NULL) {
		display->direct = 1;
		return 0;
	}

	/* A screen outside RAM: a device view of its pages. */
	error = kern_device_map(display->screen.physical_base - offset, display->view_bytes, KERN_DEVICE_UNCACHED, &view);
	if (error != 0)
		return error;

	/* Succeeded: the CPU can write the screen. */
	display->pixels = (uint8_t *)view + offset;
	display->direct = 0;
	return 0;
}

/*
 * Reads the VPU's first plane and allows flipping when it shows the boot
 * framebuffer from a linear buffer whose address the plane's word holds.
 */
static void
find_plane(
	struct display *display,
	const struct drv_fdt *fdt)
{
	static const char *const names[] = {
		"amlogic, meson-s7d-vpu",
		"amlogic,meson-s7d-vpu",
	};
	struct mali_osd_plane *plane;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	unsigned index;
	void *mapped;
	int shows;
	int error;

	/* Starts with copies only. */
	display->flip = 0;
	display->vpu = NULL;

	/* The VPU's node, by the vendor's name with or without its space. */
	error = ENODEV;
	for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
		error = drv_fdt_find_compatible(fdt, names[index], DRV_FDT_NO_NODE, &node);
		if (error == 0)
			break;
	}

	if (error != 0) {
		kern_logf("mali: display: no VPU in the device tree; frames are copied\n");
		return;
	}

	/* Its register window, large enough for the plane's registers. */
	error = drv_fdt_reg(fdt, node, 0U, &address, &size);
	if (error != 0 || size < MALI_OSD_WINDOW_BYTES) {
		kern_logf("mali: display: VPU window not usable; frames are copied\n");
		return;
	}

	error = kern_device_map(address, MALI_OSD_WINDOW_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("mali: display: VPU window not mapped (%d); frames are copied\n", error);
		return;
	}

	display->vpu = mapped;

	/* The plane's words. */
	plane = &display->plane;
	plane->control = kern_mmio_read32((const volatile void *)(display->vpu + MALI_OSD_CONTROL));
	plane->format = kern_mmio_read32((const volatile void *)(display->vpu + MALI_OSD_FORMAT));
	plane->window_x = kern_mmio_read32((const volatile void *)(display->vpu + MALI_OSD_WINDOW_X));
	plane->window_y = kern_mmio_read32((const volatile void *)(display->vpu + MALI_OSD_WINDOW_Y));
	plane->address = kern_mmio_read32((const volatile void *)(display->vpu + MALI_OSD_ADDRESS));
	plane->stride = kern_mmio_read32((const volatile void *)(display->vpu + MALI_OSD_STRIDE));

	/* Says what the plane holds, for the first look at the board. */
	kern_logf("mali: display: OSD1 control %08x format %08x window %08x %08x address %08x stride %08x\n",
		  (unsigned)plane->control,
		  (unsigned)plane->format,
		  (unsigned)plane->window_x,
		  (unsigned)plane->window_y,
		  (unsigned)plane->address,
		  (unsigned)plane->stride);

	/* The plane must read a linear buffer from its corner. */
	error = mali_osd_decode(plane);
	if (error != 0) {
		kern_logf("mali: display: OSD1 does not show a linear buffer (%d); frames are copied\n", error);
		return;
	}

	/* And that buffer must be the boot framebuffer. */
	shows = mali_osd_shows(plane, display->screen.physical_base, display->screen.width, display->screen.height, display->screen.pitch);
	if (!shows) {
		kern_logf("mali: display: OSD1 shows %llx %ux%u pitch %u, not the boot framebuffer; frames are copied\n",
			  (unsigned long long)plane->buffer,
			  plane->width,
			  plane->height,
			  plane->pitch);
		return;
	}

	/* Succeeded: the plane can be pointed at a resource and back. */
	display->framebuffer_word = plane->address;
	display->flip = 1;
}

/* Blanks every visible row of the screen.  The caller holds the mutex. */
static void
clear_screen(
	struct display *display)
{
	uint32_t row;

	/* Each row's visible pixels, opaque black, top to bottom. */
	for (row = 0U; row < display->screen.height; row++)
		kern_memset(display->pixels + (size_t)row * display->screen.pitch, 0, (size_t)display->screen.width * DISPLAY_PIXEL_BYTES);

	/* The plane reads memory, not the caches. */
	if (display->direct)
		kern_dcache_clean_range(display->pixels, (size_t)display->screen.height * display->screen.pitch);
}

/*
 * Copies a frame onto the screen, clipped to its top left, swapping red
 * and blue when the screen's order is not the frame's and setting the
 * alpha byte.  The caller holds the mutex; the core checked the frame
 * lies inside the resource.
 */
static void
copy_frame(
	struct display *display,
	const struct display_resource *resource,
	const struct gpu_display_present *request)
{
	const uint32_t *source;
	uint32_t *destination;
	uint32_t width;
	uint32_t height;
	uint32_t pixel;
	uint32_t row;
	uint32_t column;
	int swap;

	/* The part of the frame the screen has room for. */
	width = request->width;
	if (width > display->screen.width)
		width = display->screen.width;

	height = request->height;
	if (height > display->screen.height)
		height = display->screen.height;

	/* An RGBA frame swaps onto a BGRX screen, a BGRA frame onto an RGBX one. */
	swap = 0;
	if (request->format == GPU_PIXEL_RGBA8888 && !display->rgbx)
		swap = 1;
	if (request->format == GPU_PIXEL_BGRA8888 && display->rgbx)
		swap = 1;

	/* Every row, top to bottom, each pixel opaque. */
	for (row = 0U; row < height; row++) {
		source = (const uint32_t *)(const void *)(resource->address + request->offset + (uint64_t)row * request->stride);
		destination = (uint32_t *)(void *)(display->pixels + (size_t)row * display->screen.pitch);
		for (column = 0U; column < width; column++) {
			pixel = source[column];
			if (swap)
				pixel = (pixel & 0xff00ff00U) | ((pixel & 0x00ff0000U) >> 16) | ((pixel & 0x000000ffU) << 16);
			destination[column] = pixel | DISPLAY_OPAQUE;
		}
	}

	/* The plane reads memory, not the caches. */
	if (display->direct)
		kern_dcache_clean_range(display->pixels, (size_t)height * display->screen.pitch);
}

/*
 * Reports whether the frame's corners and centre carry a full alpha byte
 * (the top byte in both packed formats).
 */
static int
frame_looks_opaque(
	const struct display_resource *resource,
	const struct gpu_display_present *request)
{
	const uint32_t *row;
	uint32_t rows[3];
	uint32_t columns[3];
	unsigned y;
	unsigned x;

	/* The first, middle and last rows and columns. */
	rows[0] = 0U;
	rows[1] = request->height / 2U;
	rows[2] = request->height - 1U;
	columns[0] = 0U;
	columns[1] = request->width / 2U;
	columns[2] = request->width - 1U;

	/* Every sampled pixel must be opaque. */
	for (y = 0U; y < 3U; y++) {
		row = (const uint32_t *)(const void *)(resource->address + request->offset +
						       (uint64_t)rows[y] * request->stride);
		for (x = 0U; x < 3U; x++) {
			if ((row[columns[x]] & DISPLAY_OPAQUE) != DISPLAY_OPAQUE)
				return 0;
		}
	}

	/* Succeeded: the samples are opaque. */
	return 1;
}

/*
 * Reports whether the plane can read a frame from the resource itself:
 * flipping is allowed, and the frame has the screen's extent, row pitch
 * and byte order at a place the plane's address word names.  The caller
 * holds the mutex.
 */
static int
can_flip(
	const struct display *display,
	const struct display_resource *resource,
	const struct gpu_display_present *request)
{
	uint64_t address;
	int order_rgbx;

	/* The plane was found showing the framebuffer. */
	if (!display->flip)
		return 0;

	/* The screen's extent and the plane's row pitch. */
	if (request->width != display->screen.width || request->height != display->screen.height)
		return 0;
	if (request->stride != display->plane.pitch)
		return 0;

	/* The screen's byte order: the plane does not swap. */
	order_rgbx = 0;
	if (request->format == GPU_PIXEL_RGBA8888)
		order_rgbx = 1;
	if (order_rgbx != display->rgbx)
		return 0;

	/* A start the address word names, wholly below 4 GiB. */
	address = resource->physical + request->offset;
	if ((address & (DISPLAY_SCAN_ALIGNMENT - 1U)) != 0U)
		return 0;
	if (address + (uint64_t)request->stride * request->height - 1U > DISPLAY_SCAN_LIMIT)
		return 0;

	/*
	 * The plane blends by the alpha byte, so a frame whose alpha is not
	 * full would show through; such a frame is copied, which makes every
	 * pixel opaque.  A few pixels (the corners and the centre) stand for
	 * the frame.
	 */
	if (!frame_looks_opaque(resource, request))
		return 0;

	/* Succeeded: the frame can be flipped. */
	return 1;
}

/*
 * Points the plane at a buffer and waits one frame for it to read the
 * new one.  The plane's previous resource is let go of: when its owner
 * destroyed it, it is stored through freed for the caller to free after
 * the mutex.  The caller holds the mutex.
 */
static void
point_plane(
	struct display *display,
	uint64_t address,
	struct display_resource *front,
	struct display_resource **freed)
{
	struct display_resource *previous;
	uint32_t word;
	int error;

	/* The address word; the framebuffer's is the one the plane was found with. */
	word = display->framebuffer_word;
	if (front != NULL) {
		error = mali_osd_address_word(address, &word);
		if (error != 0)
			return;
	}

	/* Nothing to do when the plane already reads the buffer. */
	previous = display->front;
	if (previous == front)
		return;

	/* The new address, then a frame for the plane to take it. */
	kern_io_write_barrier();
	kern_mmio_write32((volatile void *)(display->vpu + MALI_OSD_ADDRESS), word);
	sched_sleep(sched_ticks() + kern_ms_to_ticks(DISPLAY_FRAME_MS) + 1U);
	display->front = front;

	/* The previous resource is free again; one its owner destroyed goes to the caller. */
	if (previous != NULL && previous->destroyed)
		*freed = previous;
}

/*
 * Ends the lease: the plane goes back to the framebuffer and the text
 * console draws its retained text again.  The caller holds the mutex.
 */
static void
end_lease(
	struct display *display,
	struct display_resource **freed)
{
	/* The framebuffer before the console draws on it. */
	if (display->front != NULL)
		point_plane(display, display->screen.physical_base, NULL, freed);

	display->owner = NULL;
	display->lease = 0U;
	display->sequence = 0U;
	kern_text_resume();
}

/* Frees one resource's run, or its hold on an imported object, and its record. */
static void
free_resource(
	struct display_resource *resource)
{
	/* An import gives its hold back; a run its memory. */
	if (resource->bo != NULL)
		mali_render_scanout_drop(resource->bo);
	else
		(void)kern_pmem_free(&resource->run);

	/* The record. */
	kern_free(resource);
}

/*
 * Checks a shared frame: an import, shown in the layout it was exported
 * with.
 */
static int
shared_frame_valid(
	const struct display_resource *resource,
	const struct gpu_display_present *request)
{
	/* Only an import is a shared frame. */
	if (resource->bo == NULL)
		return EINVAL;

	/* The exported layout is the one shown. */
	if (request->width != resource->image.width ||
	    request->height != resource->image.height ||
	    request->format != resource->image.format ||
	    request->stride != resource->image.stride ||
	    request->offset != resource->image.offset)
		return EINVAL;

	/* Succeeded: the frame lies in the object (checked at import). */
	return 0;
}
