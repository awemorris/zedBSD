/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display node of the PowerVR driver: the boot framebuffer.
 *
 * The VisionFive 2 has no display controller driver of its own yet; the
 * loader leaves the screen on and hands over its framebuffer, which the
 * kernel's text console draws on.  This node gives that screen to a
 * session through the display operations (query, mode, claim, release,
 * present, wait): one output, one plane, one mode -- the loader's extent,
 * reported at 60 Hz -- and one lease at a time.
 *
 * While a lease is held the text console does not draw; the screen is
 * cleared for the lease's frames, and given back to the console when the
 * lease is released, also when its holder closes or exits without
 * releasing it.  The console draws its retained text again only when no
 * flip of the system has ended it (the GPU framework ends it at the first
 * flip of any node).
 *
 * A presentation copies the frame with the CPU from a resource this node
 * allocated (ordinary cacheable RAM the session writes or maps) onto the
 * framebuffer, row by row, clipped to the screen, swapping red and blue
 * when the screen's order is not the frame's.  The framebuffer is written
 * through the direct map and then cleaned out of the caches, as the text
 * console draws it; a framebuffer outside the direct map is written
 * through an uncached device view instead.  The presentation has
 * completed on the screen when it returns.
 *
 * A render buffer is shown through the scanout import: the render node's
 * private lookup finds and retains the buffer behind the exported image,
 * and a shared (BLOB) presentation copies its frame with the CPU from the
 * buffer's uncached kernel view, one aligned word at a time.  The present
 * request carries no damage, so the whole frame (clipped) is copied.  A
 * buffer the lookup does not know (another driver's) is refused.
 *
 * When the display controller's screen could be taken over (dc8200.c) and
 * dc8200.zerocopy=off is not given, a shared frame of the
 * loader's extent and row pitch, in a contiguous buffer below 4 GiB, is
 * not copied: the controller's plane is pointed at the buffer itself.
 * Such a presentation returns once the plane counts as taken, two
 * vertical blanks after the commit.  A buffer stays held while the
 * controller reads it or may still read it (the front and the pending
 * commit), also after its owner destroyed it; it is freed by the thread
 * that drops its last hold.  Any other frame is copied onto the
 * framebuffer as before, and when a buffer was on the plane the copy is
 * made first and the plane is pointed back at the framebuffer after.  A
 * commit not taken because the blanks stopped for 100 ms puts the front
 * back and reports ENXIO, as does every presentation after a fatal stop
 * took the screen back.  The release points the plane back at the
 * framebuffer before the console draws again.
 *
 * The node is registered before the render node, so it is gpu0 and the
 * render node gpu1.  It does not depend on the GPU core: it stays when the
 * core fails to come up.
 *
 * Procedure referenced:
 * src/drivers/gpu/bcm2711/display/display-device.c and
 * src/drivers/gpu/i915/display/bootfb.c (the display operations' contract
 * on a boot framebuffer).
 */

#include <stddef.h>

#include "drivers/gpu/powervr/display/dc8200-flip.h"
#include "drivers/gpu/powervr/powervr.h"
#include "drivers/gpu/powervr/render.h"
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
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

/* The one output's identifier and generation; the screen never changes. */
#define POWERVR_DISPLAY_ID		1U
#define POWERVR_DISPLAY_GENERATION	1U

/* The refresh reported for the loader's screen, which does not tell its own. */
#define POWERVR_DISPLAY_REFRESH		60000U

/* The resources one session may hold, and the largest one (64 MiB). */
#define POWERVR_DISPLAY_RESOURCES	16U
#define POWERVR_DISPLAY_RESOURCE_BYTES	(64ULL << 20)

/* The page resources are allocated in, and the framebuffer's view is made in. */
#define POWERVR_DISPLAY_PAGE_BYTES	4096U

/*
 * The flip report: how often the counts of the frames scanned directly are
 * written to the log (milliseconds), and how many bins its histograms
 * have (blanks waited: 1, 2, 3, 4 or more; the commit's place after a
 * blank: under 4, 4 to 8, 8 to 12, 12 to 14 and 14 ms or more).
 */
#define POWERVR_FLIP_REPORT_MS		5000U

/*
 * Whether the flip report is kept and written (1) or not (0).  Off by
 * default: the report is written to the serial console at its speed from
 * the present's thread, under the display's mutex.  A measurement build
 * turns it on (ZEDBSD_TEST_CPPFLAGS += -DPOWERVR_FLIP_REPORT=1).
 */
#ifndef POWERVR_FLIP_REPORT
#define POWERVR_FLIP_REPORT		0
#endif
#define POWERVR_FLIP_WAITED_BINS	4U
#define POWERVR_FLIP_PHASE_BINS		5U

/* The bytes of one pixel of the screen and of a frame. */
#define POWERVR_DISPLAY_PIXEL_BYTES	4U

/*
 * What the display controller needs of a buffer it scans directly: rows
 * starting on 32-byte boundaries, a row pitch its 16-bit register holds,
 * and every byte below 4 GiB.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_drv.c (pitch_alignment 32) and
 * vs_dc_hw.h (struct dc_hw_fb: the stride is 16 bits; the address 32).
 */
#define POWERVR_DISPLAY_SCAN_ALIGNMENT	32U
#define POWERVR_DISPLAY_SCAN_STRIDE_MAX	65535U
#define POWERVR_DISPLAY_SCAN_LIMIT	0x100000000ULL

/* How many buffers one presentation or release can let go of: two for each of up to four plane changes. */
#define POWERVR_DISPLAY_FREED_MAX	8U

/*
 * The cursor plane's images: 64 x 64 ARGB, below 4 GiB, written through
 * RAM's uncached alias; three, so the one written is neither shown nor
 * the one shown before it.
 */
#define POWERVR_DISPLAY_CURSOR_IMAGES	3U
#define POWERVR_DISPLAY_CURSOR_BYTES	(GPU_DISPLAY_CURSOR_MAX * GPU_DISPLAY_CURSOR_MAX * 4U)

/* The name a query reports, and the driver name of the node. */
#define POWERVR_DISPLAY_NAME		"powervr bootfb"
#define POWERVR_DISPLAY_DRIVER		"powervr-display"

/* The capabilities of the node: its own storage, copies, CPU views and the display. */
#define POWERVR_DISPLAY_CAPABILITIES \
	(GPU_CAP_RESOURCE | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_DISPLAY)

/*
 * One open of the node.
 *
 * Made by open and freed by close, after the core has destroyed every
 * resource of the open; the count is kept under the node's mutex.
 */
struct powervr_display_session {
	uint32_t resources;
};

/*
 * One resource of a session: a run of ordinary cacheable RAM, or an
 * imported render buffer.
 *
 * Made by resource_create (a run) or import_image (a render buffer, which
 * the resource holds a reference on) and freed by resource_destroy; the
 * memory and its address do not change in between.
 */
struct powervr_display_resource {
	/*
	 * The display controller's holds on the resource (kept under the
	 * node's mutex).  While any is left the resource outlives its
	 * destruction by the session; its owner is then never looked at
	 * again, as the session may be gone.
	 */
	struct powervr_flip_hold hold;

	/*
	 * Nonzero when the controller may scan the imported buffer itself;
	 * frame then says where and how.  Fixed at import.
	 */
	int direct;
	struct drv_powervr_dc_frame frame;

	struct powervr_display_session *owner;
	struct kern_pmem run;
	uint8_t *address;
	uint64_t bytes;

	/* The imported render buffer (NULL for a run) and the image it was exported with. */
	struct powervr_buffer *buffer;
	struct gpu_image_descriptor image;
};

/*
 * The node: the screen, how the CPU writes it, and the lease.
 *
 * Filled once by drv_powervr_display_attach() on the boot CPU; afterwards
 * the lease fields change only under the mutex, and the screen fields do
 * not change.
 */
struct powervr_display {
	/* Nonzero once the screen is known and the node is registered. */
	int ready;

	/* The loader's screen as the kernel reads it. */
	struct kern_boot_framebuffer screen;

	/* Nonzero when the screen's bytes are R, G, B, X; zero for B, G, R, X. */
	int rgbx;

	/* The CPU's view of the screen, and whether it is the direct map (cleaned after writes). */
	uint8_t *pixels;
	int direct;

	/*
	 * The view this file made, and its extent: RAM's uncached alias
	 * (alias set), or a device view of a screen outside RAM.
	 */
	void *view;
	size_t view_bytes;
	int alias;

	/* Protects the lease fields below. */
	struct mutex mutex;

	/* The session holding the lease (NULL when none), its lease, and the next lease's number. */
	struct powervr_display_session *owner;
	uint64_t lease;
	uint64_t next_lease;

	/* Frames shown under the lease, and the tick the newest was shown at. */
	uint64_t sequence;
	uint64_t present_tick;

	/* The core's handle of the registered node. */
	struct drv_gpu_device *gpu;

	/* Nonzero when the display controller counts vertical blanks: its plane can be changed. */
	int scanout;

	/* Nonzero when the lease may scan buffers directly (unless dc8200.zerocopy=off, read at claim). */
	int zero_copy;

	/*
	 * Why the last frame was copied rather than scanned directly ("" when
	 * it was scanned), and the route the log last named under this lease
	 * (-1 before the first frame); a change of route is logged once.
	 */
	const char *copy_reason;
	int logged_route;

	/*
	 * The plane's buffers, under the mutex.  Flipping is set while a
	 * thread waits, without the mutex, for a commit to be taken; another
	 * change waits on flip_done until it is over.
	 */
	struct powervr_flip flip;
	int flipping;
	struct wait_queue flip_done;

	/*
	 * The flip report, under the mutex: since report_tick, the frames
	 * scanned directly, how many commits counted as early, how many blanks
	 * each waited from its commit to its taking, where in its frame each
	 * commit was written, and the time from the present's entry to its
	 * commit (the change before it) and from the commit to its return
	 * (microseconds, sum and longest).  present_entry_us is the present
	 * under way's entry.
	 */
	uint64_t report_tick;
	uint32_t report_presents;
	uint32_t report_early;
	uint32_t report_waited[POWERVR_FLIP_WAITED_BINS];
	uint32_t report_phases[POWERVR_FLIP_PHASE_BINS];
	uint64_t report_queue_us;
	uint64_t report_queue_most_us;
	uint64_t report_take_us;
	uint64_t report_take_most_us;
	uint64_t report_period_us;
	uint64_t present_entry_us;

	/*
	 * The cursor plane, under the mutex: its images, made at the
	 * first image set, their uncached views, the one shown or last set
	 * (-1 before any), whether it is shown, and its corner.
	 */
	struct kern_pmem cursor_runs[POWERVR_DISPLAY_CURSOR_IMAGES];
	uint32_t *cursor_views[POWERVR_DISPLAY_CURSOR_IMAGES];
	int cursor_image;
	int cursor_shown;
	int32_t cursor_x;
	int32_t cursor_y;
	uint64_t cursor_sets;
	uint64_t cursor_moves;
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
static int output_cursor_set(void *device, void *session, void *resource, const struct gpu_display_cursor_image *request);
static int output_cursor_move(void *device, void *session, const struct gpu_display_cursor_move *request);
static int output_cursor_hide(void *device, void *session, const struct gpu_display_cursor_hide *request);
static int cursor_images(struct powervr_display *display);
static void cursor_off(struct powervr_display *display);

/* The display operations of the node. */
static const struct drv_gpu_display_ops display_operations = {
	.query = output_query,
	.mode = output_mode,
	.claim = output_claim,
	.release = output_release,
	.present = output_present,
	.wait = output_wait,
	.cursor_set = output_cursor_set,
	.cursor_move = output_cursor_move,
	.cursor_hide = output_cursor_hide,
};

/* The scanout operations: the node's role, its constraints, and the import that waits for the render node. */
static const struct drv_gpu_scanout_ops scanout_operations = {
	.query_device = output_device,
	.constraints = output_constraints,
	.import_image = import_image,
};

/* The operations of the node. */
static const struct drv_gpu_ops node_operations = {
	.version = DRV_GPU_INTERFACE_VERSION,
	.size = sizeof(struct drv_gpu_ops),
	.capabilities = POWERVR_DISPLAY_CAPABILITIES,
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
 * The board's one display node, set up by drv_powervr_display_attach().
 */
static struct powervr_display board_display;

static int screen_valid(const struct kern_boot_framebuffer *screen);
static int map_screen(struct powervr_display *display);
static void unmap_screen(struct powervr_display *display);
static void clear_screen(struct powervr_display *display);
static void flush_screen(struct powervr_display *display, uint32_t rows);
static void show_frame(struct powervr_display *display, const struct powervr_display_resource *resource, const struct gpu_display_present *request);
static void show_shared_frame(struct powervr_display *display, const struct powervr_display_resource *resource, const struct gpu_display_present *request);
static int shared_frame_valid(const struct powervr_display_resource *resource, const struct gpu_display_present *request);
static void end_lease(struct powervr_display *display, struct powervr_display_resource **freed, unsigned *count);
static int scan_directly(struct powervr_display *display, const struct powervr_display_resource *resource, const struct gpu_display_present *request);
static int change_plane(struct powervr_display *display, struct powervr_display_resource *resource, struct powervr_display_resource **freed, unsigned *count);
static int await_plane(struct powervr_display *display, struct powervr_display_resource **freed, unsigned *count);
static int settle_lost(struct powervr_display *display, struct powervr_display_resource **freed, unsigned *count);
static void wait_flip_over(struct powervr_display *display);
static uint64_t flip_now_us(void);
static void flip_count(struct powervr_display *display, const struct drv_powervr_dc_commit_note *note, uint64_t committed_us, uint64_t reached);
static void flip_report(struct powervr_display *display);
static void collect_freed(struct powervr_flip_hold **taken, unsigned taken_count, struct powervr_display_resource **freed, unsigned *count);
static struct powervr_display_resource *resource_of_hold(struct powervr_flip_hold *hold);
static void free_resource(struct powervr_display_resource *resource);
static void free_resources(struct powervr_display_resource **freed, unsigned count);
static void display_scanout_start(struct powervr_display *display);
static void check_direct_scan(struct powervr_display_resource *resource, const struct gpu_image_descriptor *image, const struct drv_gpu_scanout_backing *backing);
static int zero_copy_requested(void);

/*
 * Registers the display node on the loader's screen.
 */
int
drv_powervr_display_attach(
	void)
{
	struct powervr_display *display;
	int valid;
	int error;

	/* Refuses a second node. */
	display = &board_display;
	if (display->ready)
		return EALREADY;

	/* Reads the loader's screen; a boot without one has no display node. */
	error = kern_boot_framebuffer("riscv64.framebuffer", &display->screen);
	if (error != 0) {
		kern_logf("powervr: no display: no boot framebuffer (%d)\n", error);
		return error;
	}

	/* Refuses a screen the copies cannot write. */
	valid = screen_valid(&display->screen);
	if (!valid) {
		kern_logf("powervr: no display: framebuffer %ux%u pitch %u format %u not usable\n",
			  display->screen.width,
			  display->screen.height,
			  display->screen.pitch,
			  display->screen.format);
		return ENODEV;
	}

	display->rgbx = 0;
	if (display->screen.format == KERN_BOOT_FRAMEBUFFER_RGBX8888)
		display->rgbx = 1;

	/* Makes the CPU's view of the screen. */
	error = map_screen(display);
	if (error != 0) {
		kern_logf("powervr: no display: framebuffer map failed (%d)\n", error);
		return error;
	}

	/* The lease's lock; the lease starts free and lease numbers start at one. */
	error = mutex_init(&display->mutex, LOCK_RANK_DEVICE, "powervr-display");
	if (error != 0) {
		unmap_screen(display);
		kern_logf("powervr: no display: lock failed (%d)\n", error);
		return error;
	}

	display->owner = NULL;
	display->lease = 0U;
	display->next_lease = 1U;

	/* The plane starts on the loader's framebuffer, with no change under way. */
	powervr_flip_init(&display->flip);
	display->flipping = 0;
	waitq_init(&display->flip_done, "powervr-display flip");
	display->zero_copy = 0;
	display->cursor_image = -1;
	display->cursor_shown = 0;

	/* Publishes the node; before the render node, so it is the first GPU device. */
	error = drv_gpu_register(&node_operations, display, &display->gpu);
	if (error != 0) {
		unmap_screen(display);
		kern_logf("powervr: no display: register failed (%d)\n", error);
		return error;
	}

	/*
	 * Starts counting the display controller's frames when its screen can
	 * be taken over; the plane is changed only by frames that were counted.
	 */
	display->scanout = 0;
	error = drv_powervr_dc_vblank_start();
	if (error == 0)
		display->scanout = 1;

	display->ready = 1;
	if (POWERVR_DEBUG)
		kern_logf("powervr: display %ux%u on the boot framebuffer, scanout %d (vblank start %d)\n",
			  display->screen.width,
			  display->screen.height,
			  display->scanout,
			  error);

	/* Succeeded: the screen is a display node. */
	return 0;
}

/* Creates a session without a resource or the lease. */
static int
display_open(
	void *device,
	void **result)
{
	struct powervr_display_session *session;

	(void)device;

	/* Each open starts with nothing. */
	*result = NULL;
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	*result = session;

	/* Succeeded: the core owns the session. */
	return 0;
}

/* Ends a session; its lease, if it still holds one, gives the screen back to the console. */
static void
display_close(
	void *device,
	void *private_session)
{
	struct powervr_display *display;
	struct powervr_display_session *session;
	struct powervr_display_resource *freed[POWERVR_DISPLAY_FREED_MAX];
	unsigned count;

	display = device;
	session = private_session;

	/* A holder that did not release its lease loses it here. */
	count = 0;
	mutex_lock(&display->mutex);

	if (display->owner == session)
		end_lease(display, freed, &count);

	mutex_unlock(&display->mutex);

	/* Frees the destroyed buffers the plane let go of. */
	free_resources(freed, count);

	/*
	 * The core destroyed every resource of the session before closing it;
	 * one the plane still holds no longer refers to the session.
	 */
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
	info->capabilities = POWERVR_DISPLAY_CAPABILITIES;
	info->max_resources = POWERVR_DISPLAY_RESOURCES;
	info->max_resource_bytes = POWERVR_DISPLAY_RESOURCE_BYTES;
	kern_snprintf(info->driver_name, sizeof(info->driver_name), "%s", POWERVR_DISPLAY_DRIVER);

	/* Succeeded: the limits describe the node's own storage. */
	return 0;
}

/* Allocates one storage resource of a session in ordinary cacheable RAM. */
static int
resource_create(
	void *device,
	void *private_session,
	const struct gpu_resource_create *request,
	void **result)
{
	struct powervr_display *display;
	struct powervr_display_session *session;
	struct powervr_display_resource *resource;
	uint64_t bytes;
	int error;

	display = device;
	session = private_session;
	*result = NULL;

	/* Only plain storage of a bounded size. */
	if (request->bytes == 0U || request->bytes > POWERVR_DISPLAY_RESOURCE_BYTES)
		return EINVAL;
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE || request->flags != 0U)
		return ENOTSUP;

	/* Whole pages, so a CPU view of the run maps nothing else. */
	bytes = (request->bytes + POWERVR_DISPLAY_PAGE_BYTES - 1U) & ~(uint64_t)(POWERVR_DISPLAY_PAGE_BYTES - 1U);

	/* The record first, then its memory. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	error = kern_pmem_alloc((size_t)bytes, POWERVR_DISPLAY_PAGE_BYTES, &resource->run);
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
	resource->bytes = request->bytes;

	/* Counts the resource against the session's limit. */
	mutex_lock(&display->mutex);

	if (session->resources >= POWERVR_DISPLAY_RESOURCES) {
		mutex_unlock(&display->mutex);
		(void)kern_pmem_free(&resource->run);
		kern_free(resource);
		return ENOSPC;
	}

	session->resources++;

	mutex_unlock(&display->mutex);

	*result = resource;

	/* Succeeded: the core owns the resource until resource_destroy. */
	return 0;
}

/* Frees one resource after the core has dropped every view and pin of it. */
static void
resource_destroy(
	void *device,
	void *private_session,
	void *private_resource)
{
	struct powervr_display *display;
	struct powervr_display_session *session;
	struct powervr_display_resource *resource;
	uint64_t sequence;
	int now;

	display = device;
	session = private_session;
	resource = private_resource;

	/*
	 * Gives the resource's place in the session's limit back, and frees
	 * it now unless the display controller reads it or may still read it;
	 * then the thread that drops the last hold frees it.
	 */
	sequence = 0;
	if (display->scanout)
		sequence = drv_powervr_dc_sequence();
	mutex_lock(&display->mutex);

	if (session->resources != 0U)
		session->resources--;
	now = powervr_flip_destroy(&display->flip, &resource->hold, sequence);

	mutex_unlock(&display->mutex);

	/* Frees a resource nothing on the screen reads. */
	if (now)
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
	struct powervr_display_resource *resource;

	(void)device;
	resource = private_resource;

	/* Only the owner's run, and only inside it. */
	if (resource->owner != session)
		return EINVAL;
	if (resource->buffer != NULL)
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
	struct powervr_display_resource *resource;

	(void)device;
	resource = private_resource;

	/* Only the owner's run, and only inside it. */
	if (resource->owner != session)
		return EINVAL;
	if (resource->buffer != NULL)
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
	struct powervr_display_resource *resource;

	(void)device;
	resource = private_resource;

	/* Only the owner maps its run; a render buffer is the render node's to map. */
	if (resource->owner != session)
		return EINVAL;
	if (resource->buffer != NULL)
		return ENOTSUP;

	/* Ordinary cacheable RAM, as the CPU copies of a presentation read it. */
	mapping->physical = resource->run.paddr;
	mapping->address = resource->address;
	mapping->bytes = resource->run.size;
	mapping->attributes = 0U;

	/* Succeeded: the view stays valid while the resource lives. */
	return 0;
}

/* Describes the one output: the loader's screen. */
static int
output_query(
	void *device,
	void *session,
	struct gpu_display_info *request)
{
	struct powervr_display *display;

	(void)session;
	display = device;

	/* One output. */
	request->count = 1U;
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;
	if (request->index != 0U)
		return EINVAL;

	/*
	 * Connected, FIFO, showing shared frames (an imported render buffer,
	 * scanned straight when it can be, see check_direct_scan), active
	 * while a lease has shown a frame.
	 */
	request->display_id = POWERVR_DISPLAY_ID;
	request->generation = POWERVR_DISPLAY_GENERATION;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;

	/* The cursor plane, when the controller is the driver's (counting frames). */
	if (!display->scanout)
		display_scanout_start(display);
	if (display->scanout)
		request->flags |= GPU_DISPLAY_HAS_CURSOR;
	mutex_lock(&display->mutex);

	if (display->owner != NULL && display->sequence != 0U)
		request->flags |= GPU_DISPLAY_ACTIVE;

	mutex_unlock(&display->mutex);

	/* One plane, the two packed formats, and the largest frame taken. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = POWERVR_DISPLAY_RESOURCE_BYTES;

	/* The loader's extent is the current, preferred and largest one. */
	request->current_width = display->screen.width;
	request->current_height = display->screen.height;
	request->preferred_width = display->screen.width;
	request->preferred_height = display->screen.height;
	request->max_width = display->screen.width;
	request->max_height = display->screen.height;
	request->refresh_millihz = POWERVR_DISPLAY_REFRESH;

	/* The loader does not tell the screen's physical size. */
	request->physical_width_mm = 0U;
	request->physical_height_mm = 0U;
	kern_snprintf(request->name, sizeof(request->name), "%s", POWERVR_DISPLAY_NAME);

	/* Succeeded: the output is described. */
	return 0;
}

/* Enumerates the one mode, or checks a mode: the loader's extent or a smaller one, at 60 Hz. */
static int
output_mode(
	void *device,
	void *session,
	struct gpu_display_mode *request)
{
	struct powervr_display *display;

	(void)session;
	display = device;

	/* Only the one output of this generation. */
	if (request->display_id != POWERVR_DISPLAY_ID)
		return ENOENT;
	if (request->generation != POWERVR_DISPLAY_GENERATION)
		return ESTALE;

	/* Enumeration: the loader's mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;

		request->width = display->screen.width;
		request->height = display->screen.height;
		request->refresh_millihz = POWERVR_DISPLAY_REFRESH;
		return 0;
	}

	/* Validation: an extent that fits, at the one refresh (zero asks for it). */
	if (request->operation != GPU_DISPLAY_MODE_VALIDATE)
		return EINVAL;
	if (request->width == 0U || request->height == 0U)
		return EINVAL;
	if (request->width > display->screen.width || request->height > display->screen.height)
		return EINVAL;
	if (request->refresh_millihz != 0U && request->refresh_millihz != POWERVR_DISPLAY_REFRESH)
		return EINVAL;

	request->refresh_millihz = POWERVR_DISPLAY_REFRESH;

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
	struct powervr_display *display;
	struct powervr_display_session *session;

	display = device;
	session = private_session;

	/* Only plane 0 of the one output of this generation. */
	if (request->display_id != POWERVR_DISPLAY_ID)
		return ENOENT;
	if (request->generation != POWERVR_DISPLAY_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

	/*
	 * Starts counting frames when attach could not: the board lights its
	 * own mode only after the GPU attached, so the controller was
	 * not yet read then and every present was copied.
	 */
	if (!display->scanout)
		display_scanout_start(display);

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
	request->lease = display->lease;

	/* Lets the lease scan buffers directly only when the boot asked for the test. */
	display->zero_copy = 0;
	if (display->scanout)
		display->zero_copy = zero_copy_requested();
	display->logged_route = -1;
	display->copy_reason = "";

	/* The console stops drawing before the screen is cleared for the frames. */
	kern_text_suspend();
	clear_screen(display);

	mutex_unlock(&display->mutex);

	if (POWERVR_DEBUG)
		kern_logf("powervr: display lease %llu claimed; the text console is suspended, zero copy %d (scanout %d, asked %d)\n",
			  (unsigned long long)request->lease,
			  display->zero_copy,
			  display->scanout,
			  zero_copy_requested());

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
	struct powervr_display *display;
	struct powervr_display_resource *freed[POWERVR_DISPLAY_FREED_MAX];
	unsigned count;

	display = device;

	/* Only the holder releases its own lease. */
	count = 0;
	mutex_lock(&display->mutex);

	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EINVAL;
	}

	end_lease(display, freed, &count);

	mutex_unlock(&display->mutex);

	/* Frees the destroyed buffers the plane let go of. */
	free_resources(freed, count);

	/* Succeeded: the console draws the screen again. */
	return 0;
}

/*
 * Shows a frame of the lease holder, and it is on the screen when this
 * returns: the display controller scans a shared (BLOB) frame's buffer
 * directly when it can, and otherwise the CPU copies the frame from the
 * session's run or the imported render buffer onto the framebuffer.
 * ENXIO when the output was lost or a fatal stop took the screen back.
 */
static int
output_present(
	void *device,
	void *session,
	void *private_resource,
	struct gpu_display_present *request)
{
	struct powervr_display *display;
	struct powervr_display_resource *resource;
	struct powervr_display_resource *freed[POWERVR_DISPLAY_FREED_MAX];
	uint64_t entry_us;
	unsigned count;
	int shared;
	int direct;
	int error;

	display = device;
	resource = private_resource;
	count = 0;
	entry_us = 0U;
	if (POWERVR_FLIP_REPORT)
		entry_us = flip_now_us();

	/* Only the session's own resource; the core checked the frame lies inside it. */
	if (resource->owner != session)
		return EINVAL;

	/* A shared frame comes from an imported render buffer in its exported layout, an ordinary one from a run. */
	shared = (request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U;
	if (shared) {
		error = shared_frame_valid(resource, request);
		if (error != 0)
			return error;
	} else if (resource->buffer != NULL) {
		return EINVAL;
	}

	/*
	 * Shows the frame under the mutex, after any plane change under way;
	 * the lease is checked once that is over.
	 */
	mutex_lock(&display->mutex);

	wait_flip_over(display);
	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}

	if (request->generation != POWERVR_DISPLAY_GENERATION) {
		mutex_unlock(&display->mutex);
		return ESTALE;
	}

	/* Refuses the frame while the output stays lost; a taken write-back ends that. */
	error = settle_lost(display, freed, &count);
	if (error == 0) {
		/*
		 * Points the plane at the buffer itself when it can scan it;
		 * otherwise copies the frame onto the framebuffer first and then
		 * points the plane back at the framebuffer, when a buffer was on
		 * it, so the old framebuffer is never shown.
		 */
		direct = 0;
		display->copy_reason = "not a shared frame";
		if (shared)
			direct = scan_directly(display, resource, request);

		/* Says the route when it changes: scanned directly, or copied and why. */
		if (POWERVR_DEBUG && direct != display->logged_route) {
			display->logged_route = direct;
			if (direct) {
				kern_logf("powervr: display lease %llu: frames scanned directly\n", (unsigned long long)display->lease);
			} else {
				kern_logf("powervr: display lease %llu: frames copied (%s)\n", (unsigned long long)display->lease, display->copy_reason);
			}
		}

		if (direct) {
			display->present_entry_us = entry_us;
			error = change_plane(display, resource, freed, &count);
			if (POWERVR_FLIP_REPORT)
				flip_report(display);
		} else {
			if (shared)
				show_shared_frame(display, resource, request);
			else
				show_frame(display, resource, request);
			if (display->flip.front != NULL)
				error = change_plane(display, NULL, freed, &count);
		}
	}

	/* Counts the frame shown. */
	if (error == 0) {
		display->sequence++;
		display->present_tick = sched_ticks();
		request->sequence = display->sequence;
	}

	mutex_unlock(&display->mutex);

	/* Frees the destroyed buffers the plane let go of, outside the mutex. */
	free_resources(freed, count);

	/* Reports a lost output or a fatal stop. */
	if (error != 0)
		return ENXIO;

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
	struct powervr_display *display;

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
	request->generation = POWERVR_DISPLAY_GENERATION;

	mutex_unlock(&display->mutex);

	/* Succeeded: every frame up to the sequence is on the screen. */
	return 0;
}

/* Describes the node as a display-only device; the render node is not named before it registers. */
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

/* Reports both routes: copies from a run, and shared frames imported from the render node. */
static int
output_constraints(
	void *device,
	void *session,
	struct gpu_scanout_constraints *request)
{
	(void)device;
	(void)session;

	/* Only the one output of this generation. */
	if (request->display_id != POWERVR_DISPLAY_ID)
		return ENXIO;
	if (request->generation != POWERVR_DISPLAY_GENERATION)
		return ESTALE;

	/*
	 * Copies from ordinary storage, and render buffers imported from the
	 * other node, in the two packed formats, rows of whole pixels.  The
	 * CPU reads an import, so it needs no placement of its own.
	 */
	request->flags = GPU_SCANOUT_COPY | GPU_SCANOUT_SHARED | GPU_SCANOUT_FOREIGN;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->stride_alignment = POWERVR_DISPLAY_PIXEL_BYTES;
	request->offset_alignment = POWERVR_DISPLAY_PIXEL_BYTES;
	request->placement = 0U;
	request->max_dma_address = 0U;

	/* Succeeded: the constraints of the copied route. */
	return 0;
}

/*
 * Imports a render buffer for shared frames: the render node's private
 * lookup finds the buffer behind this image and backing and retains it.
 * A backing the render node did not hand out is refused with ENOTSUP.
 */
static int
import_image(
	void *device,
	void *private_session,
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing,
	void **result)
{
	struct powervr_display *display;
	struct powervr_display_session *session;
	struct powervr_display_resource *resource;
	struct powervr_buffer *buffer;
	uint64_t bytes;
	int error;

	display = device;
	session = private_session;

	/* Owns nothing on failure. */
	*result = NULL;

	/* A linear image of whole pixels in one of the two packed formats. */
	if (image->format != GPU_PIXEL_RGBA8888 && image->format != GPU_PIXEL_BGRA8888)
		return ENOTSUP;
	if (image->tiling != GPU_IMAGE_LINEAR)
		return ENOTSUP;
	if ((image->stride % POWERVR_DISPLAY_PIXEL_BYTES) != 0U || (image->offset % POWERVR_DISPLAY_PIXEL_BYTES) != 0U)
		return EINVAL;
	if (image->width == 0U || image->height == 0U || (uint64_t)image->stride < (uint64_t)image->width * POWERVR_DISPLAY_PIXEL_BYTES)
		return EINVAL;

	/* Only a live export of the render node; the lookup takes a reference. */
	error = powervr_render_lookup(image, backing, &buffer);
	if (error != 0)
		return error;

	/* Every row inside the buffer as allocated. */
	bytes = powervr_buffer_bytes(buffer);
	if (image->offset > bytes || (uint64_t)image->stride * image->height > bytes - image->offset) {
		powervr_buffer_release(buffer);
		return EINVAL;
	}

	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL) {
		powervr_buffer_release(buffer);
		return ENOMEM;
	}

	resource->owner = session;
	resource->buffer = buffer;
	resource->image = *image;
	resource->bytes = bytes;

	/* Notes whether the display controller could scan the buffer itself. */
	check_direct_scan(resource, image, backing);

	/* Counts the import against the session's limit. */
	mutex_lock(&display->mutex);

	if (session->resources >= POWERVR_DISPLAY_RESOURCES) {
		mutex_unlock(&display->mutex);
		powervr_buffer_release(buffer);
		kern_free(resource);
		return ENOSPC;
	}

	session->resources++;

	mutex_unlock(&display->mutex);

	*result = resource;

	/* Succeeded: the resource holds the buffer until resource_destroy. */
	return 0;
}

/*
 * Checks a shared frame: an imported render buffer, shown in the layout
 * it was exported with.
 */
static int
shared_frame_valid(
	const struct powervr_display_resource *resource,
	const struct gpu_display_present *request)
{
	/* Only an import is a shared frame. */
	if (resource->buffer == NULL)
		return EINVAL;

	/* The exported layout is authoritative. */
	if (request->width != resource->image.width ||
	    request->height != resource->image.height ||
	    request->format != resource->image.format ||
	    request->stride != resource->image.stride ||
	    request->offset != resource->image.offset)
		return EINVAL;

	/* Succeeded: the frame lies in the buffer (checked at import). */
	return 0;
}

/* Reports whether the copies can write a screen: a known format and room for every row. */
static int
screen_valid(
	const struct kern_boot_framebuffer *screen)
{
	uint64_t needed;

	/* The two four-byte formats. */
	if (screen->format != KERN_BOOT_FRAMEBUFFER_RGBX8888 &&
	    screen->format != KERN_BOOT_FRAMEBUFFER_BGRX8888)
		return 0;

	/* A visible extent, and rows of whole pixels at least as long as the width. */
	if (screen->width == 0U || screen->height == 0U)
		return 0;
	if ((screen->pitch % POWERVR_DISPLAY_PIXEL_BYTES) != 0U)
		return 0;
	if ((uint64_t)screen->pitch < (uint64_t)screen->width * POWERVR_DISPLAY_PIXEL_BYTES)
		return 0;

	/* Memory for every row. */
	needed = (uint64_t)screen->pitch * screen->height;
	if (screen->size < needed)
		return 0;

	/* Succeeded: the screen can be written. */
	return 1;
}

/*
 * Makes the CPU's view of the screen, uncached so that what the CPU
 * writes is in memory for the display controller without cache
 * maintenance.  A screen in RAM is written through RAM's uncached alias:
 * the U74 has no page attribute that makes RAM uncached, so a device view
 * of RAM is still cached, and the direct map with a clean afterwards
 * showed stale cache-line pieces on the board.  A screen outside RAM gets
 * a device view.
 */
static int
map_screen(
	struct powervr_display *display)
{
	struct kern_pmem run;
	uint64_t offset;
	void *kernel;
	void *view;
	int error;

	/* The whole pages the screen lies in. */
	display->view = NULL;
	display->view_bytes = 0U;
	display->pixels = NULL;
	display->alias = 0;
	display->direct = 0;
	offset = display->screen.physical_base & (POWERVR_DISPLAY_PAGE_BYTES - 1U);
	display->view_bytes = (size_t)((offset + display->screen.size + POWERVR_DISPLAY_PAGE_BYTES - 1U) & ~(uint64_t)(POWERVR_DISPLAY_PAGE_BYTES - 1U));

	/* A screen in RAM: what the console wrote through the direct map goes to memory, then the alias. */
	kernel = kern_pmem_to_kernel(display->screen.physical_base - offset);
	if (kernel != NULL) {
		kern_dcache_clean_range(kernel, display->view_bytes);
		run.paddr = display->screen.physical_base - offset;
		run.size = display->view_bytes;
		error = kern_pmem_map_uncached(&run, &view);
		if (error != 0) {
			display->view_bytes = 0U;
			return error;
		}

		display->view = view;
		display->pixels = (uint8_t *)view + offset;
		display->alias = 1;
		return 0;
	}

	/* A screen outside RAM: a device view of its pages. */
	error = kern_device_map(display->screen.physical_base - offset, display->view_bytes, KERN_DEVICE_UNCACHED, &view);
	if (error != 0) {
		display->view_bytes = 0U;
		return error;
	}

	display->view = view;
	display->pixels = (uint8_t *)view + offset;

	/* Succeeded: the CPU can write the screen. */
	return 0;
}

/* Ends the device view of the screen, when there is one. */
static void
unmap_screen(
	struct powervr_display *display)
{
	/* RAM's alias, or the device view. */
	if (display->view != NULL && display->alias)
		(void)kern_pmem_unmap_uncached(display->view, display->view_bytes);
	else if (display->view != NULL)
		(void)kern_device_unmap(display->view, display->view_bytes);

	display->view = NULL;
	display->view_bytes = 0U;
	display->pixels = NULL;
}

/* Blanks every visible row of the screen.  The caller holds the mutex. */
static void
clear_screen(
	struct powervr_display *display)
{
	uint32_t row;

	/* Each row's visible pixels, top to bottom. */
	for (row = 0U; row < display->screen.height; row++)
		kern_memset(display->pixels + (size_t)row * display->screen.pitch, 0, (size_t)display->screen.width * POWERVR_DISPLAY_PIXEL_BYTES);

	/* The display controller reads memory, not the caches. */
	flush_screen(display, display->screen.height);
}

/* Pushes the top rows of the screen out of the caches when they were written through the direct map. */
static void
flush_screen(
	struct powervr_display *display,
	uint32_t rows)
{
	/* An uncached view wrote memory already. */
	if (!display->direct || rows == 0U)
		return;

	/* The rows written, as one range. */
	kern_dcache_clean_range(display->pixels, (size_t)rows * display->screen.pitch);
}

/*
 * Copies a frame onto the screen, clipped to its top left, swapping red
 * and blue when the screen's order is not the frame's.  The caller holds
 * the mutex; the core checked the frame lies inside the resource.
 */
static void
show_frame(
	struct powervr_display *display,
	const struct powervr_display_resource *resource,
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

	/* Every row, top to bottom. */
	for (row = 0U; row < height; row++) {
		source = (const uint32_t *)(const void *)(resource->address + request->offset + (uint64_t)row * request->stride);
		destination = (uint32_t *)(void *)(display->pixels + (size_t)row * display->screen.pitch);
		if (!swap) {
			kern_memcpy(destination, source, (size_t)width * POWERVR_DISPLAY_PIXEL_BYTES);
			continue;
		}

		/* Each pixel with its first and third bytes exchanged. */
		for (column = 0U; column < width; column++) {
			pixel = source[column];
			destination[column] = (pixel & 0xff00ff00U) | ((pixel & 0x00ff0000U) >> 16) | ((pixel & 0x000000ffU) << 16);
		}
	}

	/* The display controller reads memory, not the caches. */
	flush_screen(display, height);
}

/*
 * Copies a shared frame from an imported render buffer's uncached view
 * onto the screen, clipped to its top left, swapping red and blue when
 * the orders differ.  The view takes only plain aligned loads, so each
 * pixel is read as one word.  The caller holds the mutex.
 */
static void
show_shared_frame(
	struct powervr_display *display,
	const struct powervr_display_resource *resource,
	const struct gpu_display_present *request)
{
	const volatile uint32_t *source;
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

	/* Every row, top to bottom, a word at a time. */
	for (row = 0U; row < height; row++) {
		source = (const volatile uint32_t *)(const volatile void *)(powervr_buffer_view(resource->buffer) + request->offset + (uint64_t)row * request->stride);
		destination = (uint32_t *)(void *)(display->pixels + (size_t)row * display->screen.pitch);
		for (column = 0U; column < width; column++) {
			pixel = source[column];
			if (swap)
				pixel = (pixel & 0xff00ff00U) | ((pixel & 0x00ff0000U) >> 16) | ((pixel & 0x000000ffU) << 16);
			destination[column] = pixel;
		}
	}

	/* The display controller reads memory, not the caches. */
	flush_screen(display, height);
}

/*
 * Ends the lease and lets the text console draw its retained text again.
 *
 * The plane is pointed back at the framebuffer first, after any change
 * under way, and buffers the plane lets go of are stored through freed
 * for the caller to free.  When the plane does not take the framebuffer
 * back, its buffers stay held: leaking them is safer than freeing what
 * the controller may read.  The caller holds the mutex.
 */
static void
end_lease(
	struct powervr_display *display,
	struct powervr_display_resource **freed,
	unsigned *count)
{
	int error;

	/* Lets a change under way, and a lost output's write-back, finish first. */
	error = 0;
	wait_flip_over(display);
	if (display->flip.committing)
		error = await_plane(display, freed, count);

	/* Points the plane back at the framebuffer, which the console draws on. */
	if (error == 0 && display->flip.front != NULL)
		error = change_plane(display, NULL, freed, count);

	/* Keeps the buffers held when the controller did not take the framebuffer back. */
	if (error != 0) {
		kern_logf("powervr: display lease %llu: the plane kept a buffer; it stays held\n",
			  (unsigned long long)display->lease);
	}

	/* Writes what the flips did, for a test to check. */
	if (display->scanout) {
		if (POWERVR_DEBUG)
			kern_logf("powervr: display flips %llu, mismatches %u, lost %d\n",
				  (unsigned long long)display->flip.recorded,
				  display->flip.mismatches,
				  display->flip.lost);
		drv_powervr_dc_vblank_report();
	}

	if (POWERVR_DEBUG)
		kern_logf("powervr: display lease %llu released after %llu frame(s); the screen goes back to the text console\n",
			  (unsigned long long)display->lease,
			  (unsigned long long)display->sequence);

	/* The lease's cursor goes with it; its image is kept for the next. */
	if (POWERVR_DEBUG)
		kern_logf("powervr: display lease %llu: cursor plane sets %llu moves %llu\n",
			  (unsigned long long)display->lease, (unsigned long long)display->cursor_sets, (unsigned long long)display->cursor_moves);
	display->cursor_sets = 0;
	display->cursor_moves = 0;
	cursor_off(display);

	/* The screen is free. */
	display->owner = NULL;
	display->lease = 0U;
	display->sequence = 0U;
	display->zero_copy = 0;

	/* The console repaints the whole screen from its grid. */
	kern_text_resume();
}

/*
 * Reports whether the display controller can scan a shared frame's buffer
 * itself: the lease asked for it, the controller counts frames, the
 * buffer passed the import's checks, and the frame has the plane's extent
 * and row pitch.  The caller holds the mutex.
 */
static int
scan_directly(
	struct powervr_display *display,
	const struct powervr_display_resource *resource,
	const struct gpu_display_present *request)
{
	int ready;
	int fits;

	/* Only under a lease that asked for it. */
	display->copy_reason = "zero copy off for the lease";
	if (!display->zero_copy)
		return 0;

	/* Only a buffer the controller can reach in one run with its rows aligned. */
	display->copy_reason = "buffer not scannable (offset, stride, contiguity or 4 GiB)";
	if (!resource->direct)
		return 0;

	/* Only while the controller counts frames and no fatal stop took the screen. */
	display->copy_reason = "controller not ready";
	ready = drv_powervr_dc_scanout_ready();
	if (!ready)
		return 0;

	/* Only a frame the plane shows unscaled at the loader's extent and pitch. */
	display->copy_reason = "frame size or pitch not the plane's";
	fits = drv_powervr_dc_frame_fits(request->width, request->height, request->stride);
	if (!fits)
		return 0;

	/* Succeeded: scanned directly. */
	display->copy_reason = "";
	return 1;
}

/*
 * Points the plane at a resource's buffer, or at the framebuffer when
 * resource is NULL, and waits for the plane to take it.  The caller holds
 * the mutex and no change is under way; the mutex is let go during the
 * wait.  ENXIO when a fatal stop took the screen back or the output was
 * lost.
 */
static int
change_plane(
	struct powervr_display *display,
	struct powervr_display_resource *resource,
	struct powervr_display_resource **freed,
	unsigned *count)
{
	const struct drv_powervr_dc_frame *frame;
	struct drv_powervr_dc_commit_note note;
	struct powervr_flip_hold *hold;
	uint64_t committed_us;
	uint64_t base;
	int error;

	/* The framebuffer is the loader's plane and takes no hold. */
	frame = NULL;
	hold = NULL;
	if (resource != NULL) {
		frame = &resource->frame;
		hold = &resource->hold;
	}

	/* Writes the commit; a fatal stop refuses it. */
	error = drv_powervr_dc_commit(frame, &base);
	if (error != 0)
		return ENXIO;

	/* Holds the buffer from now until the commit after it is taken. */
	powervr_flip_commit(&display->flip, hold, base);
	committed_us = 0U;
	kern_memset(&note, 0, sizeof(note));
	if (POWERVR_FLIP_REPORT) {
		committed_us = flip_now_us();
		drv_powervr_dc_commit_noted(&note);
	}

	/* Waits for the plane to take it. */
	error = await_plane(display, freed, count);
	if (error != 0)
		return error;

	/* A frame's buffer: its commit and taking counted for the report, when it is kept. */
	if (POWERVR_FLIP_REPORT && resource != NULL)
		flip_count(display, &note, committed_us, drv_powervr_dc_sequence());

	/* Succeeded: the plane took it. */
	return 0;
}

/*
 * Waits, without the mutex, for the plane to take the pending commit,
 * then makes it the front and lets go of the former one.  When the blanks
 * stop, the output counts as lost: the pending buffer stays held, the
 * front is written back, and ENXIO is reported.  The caller holds the
 * mutex and has a commit pending.
 */
static int
await_plane(
	struct powervr_display *display,
	struct powervr_display_resource **freed,
	unsigned *count)
{
	const struct drv_powervr_dc_frame *frame;
	struct powervr_flip_hold *taken[2];
	struct powervr_flip_hold *front;
	uint64_t target;
	uint64_t reached;
	uint64_t base;
	unsigned taken_count;
	int error;

	/*
	 * Waits for the blanks with the mutex let go, so other sessions'
	 * resources come and go meanwhile; flipping keeps every other plane
	 * change out until this one is over.
	 */
	target = display->flip.base + POWERVR_FLIP_LATCH_BLANKS;
	display->flipping = 1;
	mutex_unlock(&display->mutex);

	error = drv_powervr_dc_wait(target, &reached);

	mutex_lock(&display->mutex);
	display->flipping = 0;
	waitq_wake_all(&display->flip_done);

	/* Makes the taken commit the front and collects the buffers that went with the former one. */
	if (error == 0) {
		taken_count = powervr_flip_taken(&display->flip, reached, taken);
		collect_freed(taken, taken_count, freed, count);
		return 0;
	}

	/* Marks the output lost, keeping the pending buffer held. */
	kern_logf("powervr: display output lost: no vertical blank for 100 ms at %llu\n",
		  (unsigned long long)reached);
	powervr_flip_lose(&display->flip, reached);

	/* Writes the front back, so the plane does not stay on a buffer that was not taken. */
	front = display->flip.front;
	frame = NULL;
	if (front != NULL)
		frame = &resource_of_hold(front)->frame;
	error = drv_powervr_dc_commit(frame, &base);
	if (error == 0)
		powervr_flip_commit(&display->flip, front, base);

	return ENXIO;
}

/*
 * Ends a lost output once the front's write-back was taken, letting go of
 * the buffers that went with it; ENXIO while it was not.  Nothing to do
 * while the output is not lost.  The caller holds the mutex.
 */
static int
settle_lost(
	struct powervr_display *display,
	struct powervr_display_resource **freed,
	unsigned *count)
{
	struct powervr_flip_hold *taken[2];
	uint64_t sequence;
	unsigned taken_count;
	int due;

	/* An output that is not lost needs nothing. */
	if (!display->flip.lost)
		return 0;

	/* A write-back that could not be written never ends the loss. */
	if (!display->flip.committing)
		return ENXIO;

	/* Stays lost until the write-back counts as taken. */
	sequence = drv_powervr_dc_sequence();
	due = powervr_flip_due(display->flip.base, sequence);
	if (!due)
		return ENXIO;

	/* The plane took the write-back: the output is back. */
	taken_count = powervr_flip_taken(&display->flip, sequence, taken);
	collect_freed(taken, taken_count, freed, count);
	return 0;
}

/* Waits for a plane change under way to finish.  The caller holds the mutex. */
static void
wait_flip_over(
	struct powervr_display *display)
{
	uint64_t observed;

	/* Sleeps until the change's thread is back under the mutex and done. */
	while (display->flipping) {
		observed = waitq_sequence(&display->flip_done);
		(void)mutex_wait(&display->mutex, &display->flip_done, observed, 0, 0);
	}
}

/* Gives the fine counter's time in microseconds (0 without the counter). */
static uint64_t
flip_now_us(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	bool counting;

	/* The counter, turned into microseconds. */
	counting = kern_rtc_read_counter(&counter, &frequency);
	if (!counting || frequency == 0U)
		return 0U;

	/* Succeeded: the time. */
	return counter / frequency * 1000000U + counter % frequency * 1000000U / frequency;
}

/*
 * Counts a frame scanned directly for the flip report: whether its commit
 * was early, how many blanks it waited from the count read at its commit
 * to the count once taken, where in its frame it was written, and how long
 * it waited before and after its commit.  The caller holds the mutex.
 */
static void
flip_count(
	struct powervr_display *display,
	const struct drv_powervr_dc_commit_note *note,
	uint64_t committed_us,
	uint64_t reached)
{
	uint64_t returned_us;
	uint64_t queue_us;
	uint64_t take_us;
	uint64_t waited;
	unsigned bin;

	/* The frame, and an early commit. */
	display->report_presents++;
	if (note->early)
		display->report_early++;

	/* The blanks from the commit to its taking: 1, 2, 3, or 4 and more. */
	waited = 0U;
	if (reached > note->sequence)
		waited = reached - note->sequence;
	bin = POWERVR_FLIP_WAITED_BINS - 1U;
	if (waited <= 1U)
		bin = 0U;
	else if (waited < POWERVR_FLIP_WAITED_BINS)
		bin = (unsigned)waited - 1U;
	display->report_waited[bin]++;

	/* Where the commit was written after the newest blank. */
	bin = 4U;
	if (note->since_us < 4000U)
		bin = 0U;
	else if (note->since_us < 8000U)
		bin = 1U;
	else if (note->since_us < 12000U)
		bin = 2U;
	else if (note->since_us < 14000U)
		bin = 3U;
	display->report_phases[bin]++;
	display->report_period_us = note->period_us;

	/* The waits before and after the commit. */
	returned_us = flip_now_us();
	queue_us = 0U;
	if (committed_us > display->present_entry_us)
		queue_us = committed_us - display->present_entry_us;
	take_us = 0U;
	if (returned_us > committed_us)
		take_us = returned_us - committed_us;
	display->report_queue_us += queue_us;
	if (queue_us > display->report_queue_most_us)
		display->report_queue_most_us = queue_us;
	display->report_take_us += take_us;
	if (take_us > display->report_take_most_us)
		display->report_take_most_us = take_us;
}

/*
 * Writes the flip report to the log every POWERVR_FLIP_REPORT_MS while
 * frames are scanned directly, then starts the next.  The caller holds the
 * mutex.
 */
static void
flip_report(
	struct powervr_display *display)
{
	uint64_t now;
	uint32_t presents;

	/* Not yet time, or nothing to say. */
	now = sched_ticks();
	if (now - display->report_tick < kern_ms_to_ticks(POWERVR_FLIP_REPORT_MS))
		return;
	presents = display->report_presents;
	display->report_tick = now;
	if (presents == 0U)
		return;

	/* The counts, then cleared for the next. */
	kern_logf("powervr: display flips: %u, early %u, taken after 1/2/3/4+ blanks %u/%u/%u/%u, "
		  "committed 0-4/4-8/8-12/12-14/14+ ms after a blank %u/%u/%u/%u/%u, "
		  "before the commit %llu/%llu us, commit to taken %llu/%llu us (average/longest), period %llu us\n",
		  presents, display->report_early,
		  display->report_waited[0], display->report_waited[1], display->report_waited[2], display->report_waited[3],
		  display->report_phases[0], display->report_phases[1], display->report_phases[2], display->report_phases[3], display->report_phases[4],
		  (unsigned long long)(display->report_queue_us / presents), (unsigned long long)display->report_queue_most_us,
		  (unsigned long long)(display->report_take_us / presents), (unsigned long long)display->report_take_most_us,
		  (unsigned long long)display->report_period_us);
	display->report_presents = 0U;
	display->report_early = 0U;
	kern_memset(display->report_waited, 0, sizeof(display->report_waited));
	kern_memset(display->report_phases, 0, sizeof(display->report_phases));
	display->report_queue_us = 0U;
	display->report_queue_most_us = 0U;
	display->report_take_us = 0U;
	display->report_take_most_us = 0U;
}

/* Adds the buffers a taken commit let go of to the caller's list of resources to free. */
static void
collect_freed(
	struct powervr_flip_hold **taken,
	unsigned taken_count,
	struct powervr_display_resource **freed,
	unsigned *count)
{
	unsigned index;

	/* Appends each one while the list has room; the list is sized for the worst case. */
	for (index = 0; index < taken_count; index++) {
		if (*count >= POWERVR_DISPLAY_FREED_MAX)
			break;

		freed[*count] = resource_of_hold(taken[index]);
		(*count)++;
	}
}

/* Finds the resource a hold is embedded in. */
static struct powervr_display_resource *
resource_of_hold(
	struct powervr_flip_hold *hold)
{
	/* The hold is a member of the resource at a fixed offset. */
	return (struct powervr_display_resource *)(void *)((uint8_t *)hold - offsetof(struct powervr_display_resource, hold));
}

/* Frees a resource: its run, or its reference on the imported buffer, and the record. */
static void
free_resource(
	struct powervr_display_resource *resource)
{
	/* An import gives its reference back; a run its memory. */
	if (resource->buffer != NULL)
		powervr_buffer_release(resource->buffer);
	else
		(void)kern_pmem_free(&resource->run);

	kern_free(resource);
}

/* Frees every resource of a list; thread context, without the mutex. */
static void
free_resources(
	struct powervr_display_resource **freed,
	unsigned count)
{
	unsigned index;

	/* Frees each one in turn. */
	for (index = 0; index < count; index++)
		free_resource(freed[index]);
}

/*
 * Decides at import whether the display controller could scan the buffer
 * itself, and where: one contiguous run of pages wholly below 4 GiB, the
 * image at its start, and rows aligned and short enough for the plane's
 * pitch register.  An import that fails is still shown, by copying.
 */
static void
check_direct_scan(
	struct powervr_display_resource *resource,
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing)
{
	uint64_t index;
	uint64_t first;
	uint64_t end;

	/* Starts as copied. */
	resource->direct = 0;

	/* The image must start at the buffer's first byte. */
	if (image->offset != 0U)
		return;

	/* Rows the plane's pitch register holds, on 32-byte boundaries. */
	if ((image->stride % POWERVR_DISPLAY_SCAN_ALIGNMENT) != 0U)
		return;
	if (image->stride > POWERVR_DISPLAY_SCAN_STRIDE_MAX)
		return;

	/* Pages the controller reads as one run. */
	if (backing->pages == NULL || backing->page_count == 0U || backing->page_bytes == 0U)
		return;

	first = backing->pages[0];
	for (index = 1; index < backing->page_count; index++) {
		if (backing->pages[index] != first + index * backing->page_bytes)
			return;
	}

	/* Every row below 4 GiB, which the plane's address register reaches. */
	end = first + (uint64_t)image->stride * image->height;
	if (end > POWERVR_DISPLAY_SCAN_LIMIT || end < first)
		return;

	/* Every row inside the run. */
	if ((uint64_t)image->stride * image->height > backing->page_count * backing->page_bytes)
		return;

	/* The plane can scan the buffer at its first page. */
	resource->frame.physical = first;
	resource->frame.stride = image->stride;
	resource->frame.width = image->width;
	resource->frame.height = image->height;
	resource->frame.rgba = 0;
	if (image->format == GPU_PIXEL_RGBA8888)
		resource->frame.rgba = 1;
	resource->direct = 1;
}

/*
 * Reports whether the display may scan presented buffers directly: the
 * default, and dc8200.zerocopy=test; dc8200.zerocopy=off keeps every
 * frame copied.  The render node maps every buffer it exports around the
 * GPU's system level cache, so what the GPU wrote is in memory.
 */
static int
zero_copy_requested(
	void)
{
	const struct kern_boot_parameters *parameters;
	const char *value;
	int off;

	/* Without the boot's parameters, the default. */
	parameters = kern_boot_parameters_current();
	if (parameters == NULL)
		return 1;

	/* Without the parameter, the default. */
	value = kern_boot_parameters_value(parameters, KERN_BOOT_PARAMETER_DC8200_ZEROCOPY);
	if (value == NULL)
		return 1;

	/* Only the word off keeps the copy. */
	off = kern_strcmp(value, "off");
	if (off == 0)
		return 0;

	/* Succeeded: direct scanning. */
	return 1;
}

/*
 * Starts the controller's frame count for a display whose attach found
 * the controller not yet read, and turns direct scanout on when it runs.
 */
static void
display_scanout_start(
	struct powervr_display *display)
{
	int error;

	/* The count, once; one already started is as good. */
	error = drv_powervr_dc_vblank_start();
	if (error == 0 || error == EALREADY)
		display->scanout = 1;
	if (POWERVR_DEBUG)
		kern_logf("powervr: display scanout %d (late vblank start %d)\n", display->scanout, error);
	drv_powervr_dc_vblank_report();
}

/* Makes the cursor plane's images, once; the caller holds the mutex. */
static int
cursor_images(
	struct powervr_display *display)
{
	unsigned index;
	void *view;
	int error;

	/* Each image: RAM the controller can name, cleared, written through the uncached alias. */
	for (index = 0; index < POWERVR_DISPLAY_CURSOR_IMAGES; index++) {
		if (display->cursor_views[index] != NULL)
			continue;
		error = kern_pmem_alloc_limited(POWERVR_DISPLAY_CURSOR_BYTES, POWERVR_DISPLAY_PAGE_BYTES, POWERVR_DISPLAY_SCAN_LIMIT - 1U, 0, &display->cursor_runs[index]);
		if (error != 0)
			return error;
		error = kern_pmem_map_uncached(&display->cursor_runs[index], &view);
		if (error != 0) {
			(void)kern_pmem_free(&display->cursor_runs[index]);
			return error;
		}
		kern_memset(view, 0, POWERVR_DISPLAY_CURSOR_BYTES);
		display->cursor_views[index] = view;
	}

	/* Succeeded: the images exist. */
	return 0;
}

/* Hides the cursor; the caller holds the mutex. */
static void
cursor_off(
	struct powervr_display *display)
{
	/* Only a shown one is written. */
	if (!display->cursor_shown)
		return;
	(void)drv_powervr_dc_cursor(0, 0, 0, 0);
	display->cursor_shown = 0;
}

/*
 * Copies a cursor image into the next of the plane's images, packed at 64
 * texels a row and cleared around it (GPU_DISPLAY_CURSOR_SET); a shown
 * cursor shows it from a coming blank.
 */
static int
output_cursor_set(
	void *device,
	void *session,
	void *private_resource,
	const struct gpu_display_cursor_image *request)
{
	struct powervr_display *display;
	struct powervr_display_resource *resource;
	const volatile uint32_t *source;
	uint32_t *target;
	uint64_t span;
	uint32_t pixel;
	uint32_t row;
	uint32_t column;
	int next;
	int error;

	display = device;
	resource = private_resource;

	/* The lease's own open, from storage it owns, of a size the plane takes. */
	if (request->width == 0 || request->height == 0 || request->width > GPU_DISPLAY_CURSOR_MAX || request->height > GPU_DISPLAY_CURSOR_MAX)
		return EINVAL;
	if (request->format != GPU_PIXEL_BGRA8888 && request->format != GPU_PIXEL_RGBA8888)
		return EINVAL;
	if (request->stride < request->width * 4U || (request->stride & 3U) != 0)
		return EINVAL;
	span = (uint64_t)request->stride * (request->height - 1U) + (uint64_t)request->width * 4U;
	if (request->offset > resource->bytes || span > resource->bytes - request->offset)
		return EINVAL;

	mutex_lock(&display->mutex);

	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}
	if (resource->owner != session || !display->scanout) {
		mutex_unlock(&display->mutex);
		return resource->owner != session ? ESTALE : EOPNOTSUPP;
	}
	error = cursor_images(display);
	if (error != 0) {
		mutex_unlock(&display->mutex);
		return error;
	}

	/* The next image, cleared, then the rows (a red-first image swapped to the plane's blue-first). */
	next = (display->cursor_image + 1) % (int)POWERVR_DISPLAY_CURSOR_IMAGES;
	target = display->cursor_views[next];
	kern_memset(target, 0, POWERVR_DISPLAY_CURSOR_BYTES);
	for (row = 0; row < request->height; row++) {
		if (resource->buffer != NULL) {
			source = (const volatile uint32_t *)(const volatile void *)(powervr_buffer_view(resource->buffer) + request->offset + (uint64_t)row * request->stride);
		} else {
			source = (const volatile uint32_t *)(const volatile void *)(resource->address + request->offset + (uint64_t)row * request->stride);
		}
		for (column = 0; column < request->width; column++) {
			pixel = source[column];
			if (request->format == GPU_PIXEL_RGBA8888)
				pixel = (pixel & 0xff00ff00U) | ((pixel & 0x00ff0000U) >> 16) | ((pixel & 0x000000ffU) << 16);
			target[row * GPU_DISPLAY_CURSOR_MAX + column] = pixel;
		}
	}
	kern_io_write_barrier();
	display->cursor_image = next;
	display->cursor_sets++;

	/* A shown cursor takes the new image where it is. */
	error = 0;
	if (display->cursor_shown)
		error = drv_powervr_dc_cursor(1, display->cursor_runs[next].paddr, display->cursor_x, display->cursor_y);

	mutex_unlock(&display->mutex);

	/* Succeeded: the image is the plane's. */
	return error;
}

/* Shows the cursor with its top-left corner at a point (GPU_DISPLAY_CURSOR_MOVE). */
static int
output_cursor_move(
	void *device,
	void *session,
	const struct gpu_display_cursor_move *request)
{
	struct powervr_display *display;
	int error;

	display = device;
	mutex_lock(&display->mutex);

	/* The lease's own open, once it has set an image. */
	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}
	if (display->cursor_image < 0) {
		mutex_unlock(&display->mutex);
		return EINVAL;
	}

	/* The corner, shown from a coming blank. */
	display->cursor_x = request->x;
	display->cursor_y = request->y;
	error = drv_powervr_dc_cursor(1, display->cursor_runs[display->cursor_image].paddr, request->x, request->y);
	if (error == 0) {
		display->cursor_shown = 1;
		display->cursor_moves++;
	}

	mutex_unlock(&display->mutex);
	return error;
}

/* Hides the cursor, keeping its image (GPU_DISPLAY_CURSOR_HIDE). */
static int
output_cursor_hide(
	void *device,
	void *session,
	const struct gpu_display_cursor_hide *request)
{
	struct powervr_display *display;

	display = device;
	mutex_lock(&display->mutex);

	/* The lease's own open. */
	if (display->owner != session || display->lease != request->lease) {
		mutex_unlock(&display->mutex);
		return EBUSY;
	}
	cursor_off(display);

	mutex_unlock(&display->mutex);
	return 0;
}
