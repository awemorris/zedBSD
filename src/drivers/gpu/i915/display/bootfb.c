/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot framebuffer display of a Gen9 node.
 *
 * The driver does not set the mode of the display controller of Gen9 and
 * Gen9.5: the firmware's plane keeps scanning out the framebuffer the
 * loader handed over, and the node would have no display, so the desktop
 * could not start (VK_KHR_display found no display).  This file gives such
 * a node one display on that framebuffer.
 *
 * The display answers as the capture display does (capture.c): one output, connected, FIFO presentation of shared (BLOB) or
 * copied frames, one plane, BGRA or RGBA, one lease at a time, and one mode:
 * the firmware's extent at 60 Hz.  A presentation is synchronous: the GPU
 * copies the frame into a physically contiguous staging area of RAM (the
 * GPU cannot address the framebuffer, which is behind the aperture or is
 * another device's memory), the CPU drops its lines of the staging area,
 * and then copies the rows into the framebuffer, swapping red and blue
 * when the framebuffer's order is not the frame's.  A frame larger than the
 * framebuffer is clipped to its top-left; a smaller one leaves the rest as
 * the claim cleared it.
 *
 * The CPU writes the framebuffer through a write-combining view: the
 * aperture's (device.c) when the framebuffer lies in it, as it does on the
 * iGPU, else a view of its own.  While a lease is held the kernel's text
 * console does not draw (it would draw over the frames); it is given the
 * screen back when the lease ends, also when its holder exits or crashes.
 *
 * When the firmware's plane can be flipped and the node's step takes the
 * flips (xd9-step.h), the frames
 * are shown by flipping that plane between two buffers of the driver's
 * instead (xd9-flip.c): no CPU copy, and no tearing.  The display
 * then reports the plane's extent and refresh.  When the flips fail (a
 * flip that does not latch, a plane fault, buffers that cannot be mapped)
 * the copies come back for the rest of the boot, and the frame that found
 * the failure is copied: the desktop stays on the screen.
 *
 * XXX: one device; the staging area and the views are kept by this file.
 */

#include "internal.h"
#include "display.h"
#include "bootfb.h"
#include "xd9-backlight.h"
#include "xd9-step.h"
#include "xd9-flip.h"
#include "xd9-node.h"
#include "present.h"
#include "scanout.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../memory.h"
#include "../perf.h"
#include "../ppgtt.h"
#include "../session.h"
#include "../vk/vk-blit.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/text-display.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The one display of the node and its generation (the ids the scanout and wait paths answer for an absent panel). */
#define I915_BOOTFB_DISPLAY_ID	1U
#define I915_BOOTFB_GENERATION	1U

/* The largest frame the display accepts from a session (64 MiB, as the panel). */
#define I915_BOOTFB_MAX_FRAME_BYTES	(64ULL << 20)

/* The page the staging area is allocated and mapped in, and the framebuffer's view is made in. */
#define I915_BOOTFB_PAGE_BYTES	4096U

/* How many frames one timing line of the log sums up. */
#define I915_BOOTFB_TIMING_FRAMES	600U

/* Nanoseconds in a microsecond, for the timing lines. */
#define I915_BOOTFB_NS_PER_US		1000U

/*
 * The boot framebuffer display of the node.
 *
 * Made when the node's display is bound and kept until the display stops.
 * The lease owner's address space maps the staging area from the first
 * presentation of a lease to the end of that lease.
 */
struct i915_bootfb {
	/* Nonzero once the display is bound: the views and the staging area exist. */
	int bound;

	/* The framebuffer as the display uses it. */
	struct i915_bootfb_layout layout;

	/* The CPU's write-combining view of the visible rows. */
	volatile uint8_t *fb_cpu;

	/* The view this file made (NULL when it is the aperture's), and its page-rounded extent. */
	void *fb_view;
	size_t fb_view_bytes;

	/* The staging area: the contiguous run and its CPU view through the direct map. */
	struct kern_pmem run;
	uint8_t *staging_cpu;

	/* The address space that maps the staging area, and where; NULL while none does. */
	struct i915_ppgtt *map_vm;
	uint64_t map_va;

	/* Nonzero while presentations are flipped (xd9-flip.c) rather than copied. */
	int flip;

	/* The extent and refresh the display reports: the flipped plane's, else the framebuffer's at 60 Hz. */
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;

	/* Frames shown in all, and the copy times summed since the last timing line. */
	uint64_t frames;
	uint64_t gpu_ns;
	uint64_t cpu_ns;
	uint32_t timed;
};

/*
 * The boot framebuffer display of the one device.
 *
 * Made by the display binding on the serving thread before any session
 * exists; from then on its fields change only under the display's lease
 * mutex (the claim, the presentation, the release and a session's close),
 * and the display stop frees it.  Zero means no such display.
 */
static struct i915_bootfb i915_bootfb_display;

static int i915_bootfb_query(void *device, void *session, struct gpu_display_info *request);
static int i915_bootfb_mode(void *device, void *session, struct gpu_display_mode *request);
static int i915_bootfb_claim(void *device, void *session, struct gpu_display_claim *request);
static int i915_bootfb_release(void *device, void *session, const struct gpu_display_release *request);
static int i915_bootfb_present(void *device, void *session, void *object, struct gpu_display_present *request);
static int i915_bootfb_wait(void *device, void *session, struct gpu_display_wait *request);
static int i915_bootfb_events(void *device, void *session, uint64_t *sequence);

/*
 * The display operations of the boot framebuffer display.
 *
 * Query, mode, claim, release, present and wait are its own; the event
 * sequence is the panel's, which reads only the lease and, without a
 * panel, never touches the hardware.  There is no display control (no power,
 * no refresh), no cursor plane, and presents wait under the open's
 * admission (no DRV_GPU_DISPLAY_UNGATED).  The table never changes.
 */
static const struct drv_gpu_display_ops i915_bootfb_ops = {
	i915_bootfb_query,
	i915_bootfb_mode,
	i915_bootfb_claim,
	i915_bootfb_release,
	i915_bootfb_present,
	i915_bootfb_wait,
	i915_bootfb_events,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	0,
	NULL,
	NULL
};

static int i915_bootfb_view(struct i915_device *device, struct i915_bootfb *bootfb);
static int i915_bootfb_allocate(struct i915_bootfb *bootfb);
static int i915_bootfb_copy(struct i915_device *device, void *session, void *object, const struct gpu_display_present *request, uint32_t copy_width, uint32_t copy_height);
static void i915_bootfb_show(const struct gpu_display_present *request, uint32_t copy_width, uint32_t copy_height);
static void i915_bootfb_clear(void);
static void i915_bootfb_time(uint64_t gpu_ns, uint64_t cpu_ns, uint32_t width, uint32_t height);
static int i915_bootfb_map(struct i915_device *device, struct i915_ppgtt *vm);
static void i915_bootfb_unmap(struct i915_device *device);
static void i915_bootfb_end_lease(struct i915_device *device);
static void i915_bootfb_flip_failed(struct i915_device *device);
static int i915_bootfb_present_copied(struct i915_device *device, void *session, void *object, const struct gpu_display_present *request, uint32_t *copy_width, uint32_t *copy_height, uint64_t *gpu_ns, uint64_t *cpu_ns);

/*
 * Binds the boot framebuffer display when the node is a Gen9 one
 * without a display.
 *
 * A node of another generation, one whose display is driven, or a machine
 * without a usable framebuffer is left as it was: without a display.
 */
void
drv_i915_bootfb_bind_ops(
	struct i915_device *device,
	struct drv_gpu_ops *ops)
{
	struct kern_boot_framebuffer boot_framebuffer;
	const struct kern_boot_framebuffer *fb;
	struct i915_bootfb *bootfb;
	int error;

	bootfb = &i915_bootfb_display;

	/* Only a Gen9 node whose display was left to the firmware gets it. */
	if (device->display == NULL)
		return;
	if (!device->display->absent)
		return;
	if (device->gt.graphics_ver != 9U)
		return;

	/* Looks up the framebuffer the loader handed over, which the firmware's plane still scans out. */
	fb = &boot_framebuffer;
	error = kern_boot_framebuffer("pcat.framebuffer", &boot_framebuffer);
	if (error != 0) {
		kern_logf("i915: bootfb: the loader handed over no framebuffer; the node has no display\n");
		return;
	}

	/* Its geometry and format, and the staging area's. */
	error = drv_i915_bootfb_layout(fb, &bootfb->layout);
	if (error != 0) {
		kern_logf("i915: bootfb: the framebuffer (%ux%u stride %u format %u) cannot be used: %d; the node has no display\n",
		    fb->width,
		    fb->height,
		    fb->pitch,
		    fb->format,
		    error);
		return;
	}

	/* The CPU's write-combining view of it. */
	error = i915_bootfb_view(device, bootfb);
	if (error != 0) {
		kern_logf("i915: bootfb: the framebuffer at 0x%llx could not be mapped: %d; the node has no display\n",
		    (unsigned long long)bootfb->layout.fb_physical,
		    error);
		kern_memset(bootfb, 0, sizeof(*bootfb));
		return;
	}

	/* The staging area the GPU copies a frame into. */
	error = i915_bootfb_allocate(bootfb);
	if (error != 0) {
		kern_logf("i915: bootfb: the staging area (%llu bytes) could not be allocated: %d; the node has no display\n",
		    (unsigned long long)bootfb->layout.staging_bytes,
		    error);
		drv_i915_bootfb_fini(device);
		return;
	}

	/* The framebuffer's extent at 60 Hz, unless the firmware's plane is flipped. */
	bootfb->width = bootfb->layout.width;
	bootfb->height = bootfb->layout.height;
	bootfb->refresh_millihz = I915_BOOTFB_REFRESH_MILLIHZ;

	/* Flips the firmware's plane when the boot line asks for it and the plane allows it. */
	error = drv_i915_xd9_flip_start(device, bootfb->layout.fb_physical);
	if (error == 0) {
		bootfb->flip = 1;
		drv_i915_xd9_flip_extent(&bootfb->width, &bootfb->height, &bootfb->refresh_millihz);
	}

	/* Offers the panel's brightness when the boot line asks for it. */
	drv_i915_xd9_backlight_start(device);

	/* Offers the external displays beside a flipped panel when the boot line asks for the mode set. */
	if (bootfb->flip)
		drv_i915_xd9_node_start(device);

	/* The display and the display-only pairing (the capture node's: the same identity and formats), with their capabilities. */
	bootfb->bound = 1;
	ops->display = &i915_bootfb_ops;
	ops->scanout = &drv_i915_capture_scanout_ops;
	ops->capabilities |= GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS;

	kern_logf("i915: bootfb: %ux%u on the firmware's framebuffer at 0x%llx (stride %u, %s, %s view); presentations are %s\n",
	    bootfb->layout.width,
	    bootfb->layout.height,
	    (unsigned long long)bootfb->layout.fb_physical,
	    bootfb->layout.fb_stride_bytes,
	    (bootfb->layout.rgbx != 0) ? "RGBX" : "BGRX",
	    (bootfb->fb_view == NULL) ? "aperture" : "own",
	    (bootfb->flip != 0) ? "flipped on the firmware's plane" : "copied by the GPU and then the CPU");

	/* Says which step the display runs at, after any step that failed to start. */
	drv_i915_xd9_step_report();
}

/*
 * Reports nonzero when the node's display is the boot framebuffer
 * display.
 */
int
drv_i915_bootfb_bound(
	struct i915_device *device)
{
	UNUSED_PARAMETER(device);

	/* The display is bound until the display stops. */
	if (i915_bootfb_display.bound)
		return 1;

	/* Succeeded: the node has another display or none. */
	return 0;
}

/*
 * Ends the lease a closing session still holds.
 *
 * The staging area's mapping in the session's address space goes before
 * the space does, and the text console gets the screen back.
 */
void
drv_i915_bootfb_session_close(
	struct i915_device *device,
	void *session)
{
	struct i915_resident_display *rd;

	/* A device without a display has no lease. */
	if (device->display == NULL)
		return;

	/* The external displays' leases first, without the panel's lease mutex (xd9-node.h). */
	drv_i915_xd9_node_session_close(device, session);

	rd = &device->display->rd;

	/* A lease that was never prepared was never held. */
	if (!rd->inited)
		return;

	/* Ends the session's lease under the lease mutex. */
	mutex_lock(&rd->mutex);

	if (rd->owner == session) {
		kern_logf("i915: bootfb: the lease owner closed without releasing it\n");
		i915_bootfb_end_lease(device);
	}

	mutex_unlock(&rd->mutex);
}

/*
 * Frees the staging area and the framebuffer's view once nothing maps
 * them.
 *
 * A staging area an address space still maps is kept: the GPU could still
 * name it.
 */
void
drv_i915_bootfb_fini(
	struct i915_device *device)
{
	struct i915_bootfb *bootfb;
	int error;

	bootfb = &i915_bootfb_display;

	/* The external displays go dark, the backlight device is withdrawn, then the flip buffers go, unless the plane still shows one. */
	drv_i915_xd9_node_stop(device);
	drv_i915_xd9_backlight_stop(device);
	drv_i915_xd9_flip_fini(device);

	/* A mapped staging area stays, with everything else. */
	if (bootfb->map_vm != NULL) {
		kern_logf("i915: bootfb: XXX the staging area is still mapped at stop; it is kept\n");
		return;
	}

	/* Gives the staging run back. */
	if (bootfb->staging_cpu != NULL)
		(void)kern_pmem_free(&bootfb->run);

	/* Removes the view this file made (the aperture's belongs to the device). */
	if (bootfb->fb_view != NULL) {
		error = kern_device_unmap(bootfb->fb_view, bootfb->fb_view_bytes);
		if (error != 0)
			kern_logf("i915: bootfb: XXX the framebuffer's view could not be removed: %d\n", error);
	}

	/* Forgets the display. */
	kern_memset(bootfb, 0, sizeof(*bootfb));
}

/*
 * Checks the boot handoff's framebuffer and lays out the display's use of
 * it.
 *
 * Returns 0 with the layout, or EINVAL for an empty or oversized extent, a
 * stride shorter than a row, a format other than the two 32-bit ones, or a
 * size that does not hold every visible row.
 */
int
drv_i915_bootfb_layout(
	const struct kern_boot_framebuffer *fb,
	struct i915_bootfb_layout *layout)
{
	uint64_t fb_bytes;
	uint64_t staging_stride;

	kern_memset(layout, 0, sizeof(*layout));

	/* An empty framebuffer has nothing to show on. */
	if (fb->width == 0U || fb->height == 0U)
		return EINVAL;

	/* A side beyond the scanout layout's limit is not taken. */
	if (fb->width > I915_BOOTFB_MAX_SIDE || fb->height > I915_BOOTFB_MAX_SIDE)
		return EINVAL;

	/* The pitch counts pixels and covers at least a row. */
	if (fb->pitch < fb->width)
		return EINVAL;

	/* Only the two 32-bit orders the loader reports are drawn. */
	if (fb->format != KERN_BOOT_FRAMEBUFFER_RGBX8888 && fb->format != KERN_BOOT_FRAMEBUFFER_BGRX8888)
		return EINVAL;

	/* The visible rows lie inside what the loader says the framebuffer spans. */
	fb_bytes = (uint64_t)fb->pitch * 4U * (uint64_t)fb->height;
	if (fb_bytes > fb->size)
		return EINVAL;

	/* A staging row is a frame row rounded up to 64 bytes. */
	staging_stride = ((uint64_t)fb->width * 4U + I915_BOOTFB_STRIDE_ALIGN - 1U) & ~(uint64_t)(I915_BOOTFB_STRIDE_ALIGN - 1U);

	/* The framebuffer as the display uses it. */
	layout->width = fb->width;
	layout->height = fb->height;
	layout->fb_stride_bytes = fb->pitch * 4U;
	layout->rgbx = 0;
	if (fb->format == KERN_BOOT_FRAMEBUFFER_RGBX8888)
		layout->rgbx = 1;
	layout->fb_physical = fb->physical_base;
	layout->fb_bytes = fb_bytes;

	/* The staging area holds one frame of the framebuffer's extent, in whole pages. */
	layout->staging_stride_bytes = (uint32_t)staging_stride;
	layout->staging_bytes = (staging_stride * (uint64_t)fb->height + I915_BOOTFB_PAGE_BYTES - 1U) & ~(uint64_t)(I915_BOOTFB_PAGE_BYTES - 1U);

	/* Succeeded: the framebuffer can be shown on. */
	return 0;
}

/*
 * Clips a presented frame to the framebuffer and reports the extent that
 * is copied (the top-left part of the frame both have).
 *
 * Returns 0, or EINVAL for an empty frame.
 */
int
drv_i915_bootfb_clip(
	const struct i915_bootfb_layout *layout,
	uint32_t width,
	uint32_t height,
	uint32_t *copy_width,
	uint32_t *copy_height)
{
	/* An empty frame has nothing to show. */
	if (width == 0U || height == 0U)
		return EINVAL;

	/* The columns both have. */
	*copy_width = width;
	if (*copy_width > layout->width)
		*copy_width = layout->width;

	/* The rows both have. */
	*copy_height = height;
	if (*copy_height > layout->height)
		*copy_height = layout->height;

	/* Succeeded: the extent is known. */
	return 0;
}

/*
 * Reports nonzero when a frame of the presented format (GPU_PIXEL_*) needs
 * its red and blue swapped on the framebuffer.
 *
 * A BGRA frame is B, G, R, A in memory and an RGBA one R, G, B, A; the
 * framebuffer's X byte is the frame's alpha, which the plane ignores.
 */
int
drv_i915_bootfb_swap(
	const struct i915_bootfb_layout *layout,
	uint32_t format)
{
	/* An RGBA frame goes as it is onto an RGBX framebuffer, and needs swapping onto a BGRX one. */
	if (format == GPU_PIXEL_RGBA8888) {
		if (layout->rgbx != 0)
			return 0;

		return 1;
	}

	/* A BGRA frame needs swapping onto an RGBX framebuffer. */
	if (layout->rgbx != 0)
		return 1;

	/* Succeeded: a BGRA frame goes as it is onto a BGRX framebuffer. */
	return 0;
}

/*
 * Copies one row of pixels to the framebuffer, swapping red and blue when
 * asked.
 *
 * The row is written in order, one 32-bit store a pixel, which a
 * write-combining view gathers into whole lines.
 */
void
drv_i915_bootfb_copy_row(
	volatile uint32_t *destination,
	const uint32_t *source,
	uint32_t columns,
	int swap)
{
	uint32_t pixel;
	uint32_t column;

	/* The same bytes when the orders agree; else the first and third bytes exchanged, the second and fourth kept. */
	if (!swap) {
		for (column = 0U; column < columns; column++)
			destination[column] = source[column];
	} else {
		for (column = 0U; column < columns; column++) {
			pixel = source[column];
			destination[column] = (pixel & 0xff00ff00U) | ((pixel & 0x00ff0000U) >> 16) | ((pixel & 0x000000ffU) << 16);
		}
	}
}

/* Makes the CPU's write-combining view of the framebuffer: the aperture's when it holds it, else one of this file's own. */
static int
i915_bootfb_view(
	struct i915_device *device,
	struct i915_bootfb *bootfb)
{
	struct i915_gt *gt;
	uint64_t physical;
	uint64_t bytes;
	uint64_t page_offset;
	void *view;
	int error;

	gt = &device->gt;
	physical = bootfb->layout.fb_physical;
	bytes = bootfb->layout.fb_bytes;

	/* The iGPU's framebuffer lies in the aperture, whose view the device already has. */
	if (gt->aperture != NULL &&
	    physical >= gt->gmadr_base &&
	    bytes <= gt->gmadr_size &&
	    physical - gt->gmadr_base <= gt->gmadr_size - bytes) {
		bootfb->fb_cpu = (volatile uint8_t *)gt->aperture + (physical - gt->gmadr_base);
		bootfb->fb_view = NULL;
		bootfb->fb_view_bytes = 0U;
		return 0;
	}

	/* Another device's framebuffer (a virtual machine's display) gets a view of its own, in whole pages. */
	page_offset = physical & (uint64_t)(I915_BOOTFB_PAGE_BYTES - 1U);
	bootfb->fb_view_bytes = (size_t)((page_offset + bytes + I915_BOOTFB_PAGE_BYTES - 1U) & ~(uint64_t)(I915_BOOTFB_PAGE_BYTES - 1U));
	view = NULL;
	error = kern_device_map(physical - page_offset, bootfb->fb_view_bytes, KERN_DEVICE_WRITECOMBINE, &view);
	if (error != 0 || view == NULL) {
		bootfb->fb_view_bytes = 0U;
		return EIO;
	}

	/* The visible rows start at the framebuffer's offset in the first page. */
	bootfb->fb_view = view;
	bootfb->fb_cpu = (volatile uint8_t *)view + page_offset;

	/* Succeeded: the CPU can write the framebuffer. */
	return 0;
}

/* Makes the staging area once: one physically contiguous run the GPU's address bits reach, zeroed and flushed. */
static int
i915_bootfb_allocate(
	struct i915_bootfb *bootfb)
{
	int error;

	/* Allocates the run. */
	error = kern_pmem_alloc_limited((size_t)bootfb->layout.staging_bytes, I915_BOOTFB_PAGE_BYTES, I915_DMA_MAX_ADDRESS, 0U, &bootfb->run);
	if (error != 0)
		return error;

	/* Takes the CPU view from the direct map; a run outside managed RAM cannot be used. */
	bootfb->staging_cpu = kern_pmem_to_kernel(bootfb->run.paddr);
	if (bootfb->staging_cpu == NULL) {
		(void)kern_pmem_free(&bootfb->run);
		kern_memset(&bootfb->run, 0, sizeof(bootfb->run));
		return EFAULT;
	}

	/*
	 * Zeroes the run and flushes it: from here on the CPU holds no dirty
	 * line of it, and only the GPU writes it.
	 */
	kern_memset(bootfb->staging_cpu, 0, (size_t)bootfb->layout.staging_bytes);
	drv_i915_gt_clflush(bootfb->staging_cpu, (size_t)bootfb->layout.staging_bytes);

	/* No address space maps the area yet. */
	bootfb->map_vm = NULL;
	bootfb->map_va = 0U;

	/* Succeeded: the area can be mapped and written. */
	return 0;
}

/*
 * Describes the node's one display (the display query operation): the
 * firmware's framebuffer, connected, FIFO presentation of shared (BLOB)
 * frames, one plane, BGRA or RGBA, the firmware's extent.
 */
static int
i915_bootfb_query(
	void *device,
	void *session,
	struct gpu_display_info *request)
{
	struct i915_device *owner_device;
	struct i915_bootfb *bootfb;
	int error;

	UNUSED_PARAMETER(session);

	owner_device = device;
	bootfb = &i915_bootfb_display;

	/* The panel's display, and an external one for each sink attached (xd9-node.c). */
	request->count = 1U + drv_i915_xd9_node_count();

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* An external display. */
	if (request->index != 0U) {
		error = drv_i915_xd9_node_query(device, request->index - 1U, request);
		if (error != 0)
			return error;
		request->count = 1U + drv_i915_xd9_node_count();
		return 0;
	}

	/* The display and its state: active while a lease has frames on it. */
	request->display_id = I915_BOOTFB_DISPLAY_ID;
	request->generation = I915_BOOTFB_GENERATION;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;
	if (owner_device->display->rd.active)
		request->flags |= GPU_DISPLAY_ACTIVE;

	/* One plane, the two packed formats, and the largest source taken. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = I915_BOOTFB_MAX_FRAME_BYTES;

	/* The firmware's extent is the current, preferred and largest one. */
	request->current_width = bootfb->width;
	request->current_height = bootfb->height;
	request->preferred_width = bootfb->width;
	request->preferred_height = bootfb->height;
	request->max_width = bootfb->width;
	request->max_height = bootfb->height;
	request->refresh_millihz = bootfb->refresh_millihz;

	/* The firmware does not tell the panel's physical size. */
	request->physical_width_mm = 0U;
	request->physical_height_mm = 0U;

	kern_memcpy(request->name, "i915 bootfb", sizeof("i915 bootfb"));

	/* Succeeded: the display is described. */
	return 0;
}

/*
 * Enumerates or checks a mode of the node's display (the display mode
 * operation): the firmware's extent, and any smaller one, at 60 Hz.
 */
static int
i915_bootfb_mode(
	void *device,
	void *session,
	struct gpu_display_mode *request)
{
	struct i915_bootfb *bootfb;
	int refused;
	int external;
	int error;

	bootfb = &i915_bootfb_display;

	/* An external display's modes are the node's (xd9-node.c). */
	external = drv_i915_xd9_node_owns_display(request->display_id);
	if (external) {
		error = drv_i915_xd9_node_mode(device, session, request);
		if (error != 0)
			return error;
		return 0;
	}

	/* Only the one display of this generation exists. */
	if (request->display_id != I915_BOOTFB_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_BOOTFB_GENERATION)
		return ESTALE;

	/* Enumeration: the one mode, the firmware's. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;

		request->width = bootfb->width;
		request->height = bootfb->height;
		request->refresh_millihz = bootfb->refresh_millihz;
		return 0;
	}

	/* Validation: an extent that fits, at the display's refresh (0 asks for it). */
	refused = 0;
	if (request->width == 0U || request->height == 0U) {
		/* An empty extent shows nothing. */
		refused = 1;
	} else if (request->width > bootfb->width || request->height > bootfb->height) {
		/* The framebuffer has no room for a larger one. */
		refused = 1;
	} else if (request->refresh_millihz != 0U && request->refresh_millihz != bootfb->refresh_millihz) {
		/* The display has one refresh. */
		refused = 1;
	}

	/* A refused mode is logged with what the display takes. */
	if (refused) {
		kern_logf("i915: bootfb: mode %ux%u@%u mHz refused: up to %ux%u at %u mHz only\n",
		    request->width,
		    request->height,
		    request->refresh_millihz,
		    bootfb->width,
		    bootfb->height,
		    bootfb->refresh_millihz);
		return EINVAL;
	}

	/* The display's refresh is the mode's. */
	request->refresh_millihz = bootfb->refresh_millihz;

	/* Succeeded: the mode is shown unscaled. */
	return 0;
}

/*
 * Gives the display's lease to a session (the display claim operation).
 *
 * One lease at a time.  The text console stops drawing and the
 * framebuffer is cleared for the lease's frames.  Returns 0 with the
 * lease, ENOENT or ESTALE for another display, EINVAL for a plane other
 * than 0, or EBUSY while another session holds it.
 */
static int
i915_bootfb_claim(
	void *device,
	void *session,
	struct gpu_display_claim *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;
	int external;
	int error;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* An external display is the node's (xd9-node.c). */
	external = drv_i915_xd9_node_owns_display(request->display_id);
	if (external) {
		error = drv_i915_xd9_node_claim(device, session, request);
		if (error != 0)
			return error;
		return 0;
	}

	/* Only plane 0 of the one display of this generation exists. */
	if (request->display_id != I915_BOOTFB_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_BOOTFB_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

	/* Takes the lease under its mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Another session holds it. */
	if (rd->owner != NULL) {
		mutex_unlock(&rd->mutex);
		return EBUSY;
	}

	/* The session holds a new lease with no presentation yet. */
	rd->owner = session;
	rd->lease = rd->next_lease;
	rd->next_lease++;
	rd->sequence = 0U;
	request->lease = rd->lease;

	/*
	 * The text console stops drawing over the frames.  Copied frames
	 * start on a cleared framebuffer; flipped ones on cleared buffers,
	 * while the plane keeps the firmware's surface until the first one.
	 */
	kern_text_suspend();
	if (i915_bootfb_display.flip) {
		drv_i915_xd9_flip_claim(owner_device);
	} else {
		i915_bootfb_clear();
	}

	mutex_unlock(&rd->mutex);

	kern_logf("i915: bootfb: lease %llu claimed; the text console is suspended\n", (unsigned long long)request->lease);

	/* Succeeded: the session holds the display. */
	return 0;
}

/*
 * Ends the lease of its holder (the display release operation).
 *
 * Returns 0, or EINVAL for a session that does not hold the lease.
 */
static int
i915_bootfb_release(
	void *device,
	void *session,
	const struct gpu_display_release *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;
	int external;
	int error;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* An external display's lease is the node's (xd9-node.c). */
	external = drv_i915_xd9_node_owns_lease(request->lease);
	if (external) {
		error = drv_i915_xd9_node_release(device, session, request);
		if (error != 0)
			return error;
		return 0;
	}

	/* Ends the lease under its mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Only the lease holder releases it. */
	if (rd->owner != session || rd->lease != request->lease) {
		mutex_unlock(&rd->mutex);
		return EINVAL;
	}

	i915_bootfb_end_lease(owner_device);

	mutex_unlock(&rd->mutex);

	/* Succeeded: the lease is free. */
	return 0;
}

/*
 * Shows a frame of the lease holder (the display present operation).
 *
 * Flipped: the GPU draws the frame into the buffer the firmware's plane
 * does not show and the plane is pointed at it; the flip latches at the
 * next vertical blank, which the next presentation or a wait waits for.
 * Copied: the GPU copies the frame into the staging area and the CPU
 * copies it on to the framebuffer; both have completed when this returns.
 * A failure of the flips themselves (a flip that did not latch, a plane
 * fault, buffers that could not be mapped) brings the copies back and
 * shows this frame by copying.  Returns 0 with the presented sequence in
 * the request, EINVAL for a session that does not hold the lease, an empty
 * frame or one the GPU cannot address, ESTALE for another generation, or
 * the copy's error.
 */
static int
i915_bootfb_present(
	void *device,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;
	struct i915_bootfb *bootfb;
	uint32_t copy_width;
	uint32_t copy_height;
	uint64_t gpu_ns;
	uint64_t cpu_ns;
	int flipped;
	int flip_failed;
	int external;
	int error;

	owner_device = device;
	rd = &owner_device->display->rd;
	bootfb = &i915_bootfb_display;

	/* An external display's frame is the node's (xd9-node.c). */
	external = drv_i915_xd9_node_owns_lease(request->lease);
	if (external) {
		error = drv_i915_xd9_node_present(device, session, object, request);
		if (error != 0)
			return error;
		return 0;
	}

	/* Checks the lease and the frame, and shows it, under the lease mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Only the lease holder presents. */
	if (rd->owner != session || rd->lease != request->lease) {
		mutex_unlock(&rd->mutex);
		return EINVAL;
	}

	/* Only the one generation of the display exists. */
	if (request->generation != I915_BOOTFB_GENERATION) {
		mutex_unlock(&rd->mutex);
		return ESTALE;
	}

	/* Flips the frame onto the firmware's plane. */
	flipped = 0;
	error = 0;
	if (bootfb->flip) {
		error = drv_i915_xd9_flip_present(owner_device, session, object, request);
		if (error == 0)
			flipped = 1;
	}

	/*
	 * A frame that was not flipped failed on its own (the caller hears of
	 * it), or found the flips failed: then the copies come back for it and
	 * for the rest of the boot.
	 */
	if (bootfb->flip && !flipped) {
		flip_failed = drv_i915_xd9_flip_failed();
		if (!flip_failed) {
			mutex_unlock(&rd->mutex);
			return error;
		}

		i915_bootfb_flip_failed(owner_device);
	}

	/* A frame that was not flipped is copied by the GPU and then the CPU. */
	gpu_ns = 0U;
	cpu_ns = 0U;
	copy_width = 0U;
	copy_height = 0U;
	if (!flipped) {
		error = i915_bootfb_present_copied(owner_device, session, object, request, &copy_width, &copy_height, &gpu_ns, &cpu_ns);
		if (error != 0) {
			mutex_unlock(&rd->mutex);
			return error;
		}
	}

	/* A shown or armed presentation: the display shows this lease's frames, and the sequence advances. */
	rd->active = 1;
	rd->sequence++;
	rd->present_tick = sched_ticks();
	request->sequence = rd->sequence;
	bootfb->frames++;

	/* How long the two copies took, in the log now and then (xd9-flip.c logs the flips). */
	if (!flipped)
		i915_bootfb_time(gpu_ns, cpu_ns, copy_width, copy_height);

	mutex_unlock(&rd->mutex);

	/* Succeeded: the frame is on the screen, or its flip is armed. */
	return 0;
}

/*
 * Copies a frame on to the framebuffer: the GPU into the staging area,
 * then the CPU row by row.  Reports the extent copied and the two copies'
 * times.  Returns 0, or the clip's or the GPU copy's error.
 */
static int
i915_bootfb_present_copied(
	struct i915_device *device,
	void *session,
	void *object,
	const struct gpu_display_present *request,
	uint32_t *copy_width,
	uint32_t *copy_height,
	uint64_t *gpu_ns,
	uint64_t *cpu_ns)
{
	uint64_t started;
	uint64_t copied;
	uint64_t shown;
	int error;

	/* The part of the frame the framebuffer has room for. */
	error = drv_i915_bootfb_clip(&i915_bootfb_display.layout, request->width, request->height, copy_width, copy_height);
	if (error != 0)
		return error;

	/* The GPU copies the frame into the staging area and has completed when this returns. */
	started = drv_i915_perf_now();
	error = i915_bootfb_copy(device, session, object, request, *copy_width, *copy_height);
	if (error != 0) {
		kern_logf("i915: bootfb: the GPU copy of a %ux%u frame failed: %d\n",
		    request->width,
		    request->height,
		    error);
		return error;
	}

	/* The CPU copies the rows on to the framebuffer. */
	copied = drv_i915_perf_now();
	i915_bootfb_show(request, *copy_width, *copy_height);
	shown = drv_i915_perf_now();

	/* The two copies' times. */
	*gpu_ns = copied - started;
	*cpu_ns = shown - copied;

	/* Succeeded: the frame is on the framebuffer. */
	return 0;
}

/*
 * Waits until the newest presentation of the lease holder is on the
 * screen (the display wait operation).
 *
 * A flipped one is on the screen when its flip latches; a copied one was
 * when its presentation returned, which the panel's wait reports.  A flip
 * that does not latch, or a plane fault, brings the copies back, and the
 * frame counts as shown (the next presentation is copied).  Returns 0
 * with the completed sequence, EINVAL for a session that does not hold the
 * lease or a sequence not presented yet, or EAGAIN before the first
 * presentation.
 */
static int
i915_bootfb_wait(
	void *device,
	void *session,
	struct gpu_display_wait *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;
	uint64_t latched_tick;
	int external;
	int error;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* An external display's wait is the node's (xd9-node.c). */
	external = drv_i915_xd9_node_owns_lease(request->lease);
	if (external) {
		error = drv_i915_xd9_node_wait(device, session, request);
		if (error != 0)
			return error;
		return 0;
	}

	/* A copied presentation completed when it returned. */
	if (!i915_bootfb_display.flip) {
		error = drv_i915_present_display_wait(device, session, request);
		if (error != 0)
			return error;

		/* Succeeded: the panel's wait reported the completion. */
		return 0;
	}

	/* Reads the lease and waits for the flip under its mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Only the lease holder waits, and only for a sequence it presented. */
	if (rd->owner != session || rd->lease != request->lease || request->sequence > rd->sequence) {
		mutex_unlock(&rd->mutex);
		return EINVAL;
	}

	/* Nothing has been presented yet. */
	if (rd->sequence == 0U) {
		mutex_unlock(&rd->mutex);
		return EAGAIN;
	}

	/* The newest flip latches; the tick it did so at is its completion. */
	latched_tick = rd->present_tick;
	if (i915_bootfb_display.flip) {
		error = drv_i915_xd9_flip_wait(&latched_tick);
		if (error != 0) {
			i915_bootfb_flip_failed(owner_device);
			latched_tick = sched_ticks();
		}
	}

	/* The newest sequence, the tick it completed at, and the one generation. */
	request->completed_sequence = rd->sequence;
	request->present_time_ns = kern_ticks_to_ns(latched_tick);
	request->generation = I915_BOOTFB_GENERATION;

	mutex_unlock(&rd->mutex);

	/* Succeeded: the sequence has completed. */
	return 0;
}

/*
 * Reports the display topology's sequence (the display events operation):
 * the external displays' while they are offered (a sink that comes or
 * goes moves it), else the panel's.  Any context.
 */
static int
i915_bootfb_events(
	void *device,
	void *session,
	uint64_t *sequence)
{
	uint64_t external;
	int error;

	/* The external displays' sequence. */
	external = drv_i915_xd9_node_sequence();
	if (external != 0U) {
		*sequence = external;
		return 0;
	}

	/* The panel's. */
	error = drv_i915_display_events(device, session, sequence);
	if (error != 0)
		return error;

	/* Succeeded: the sequence is sampled. */
	return 0;
}

/*
 * Copies a presented frame into the staging area on the GPU and drops the
 * CPU's lines of the rows it wrote.
 *
 * The copy is one rectangle of the session's executor, unscaled, from the
 * frame's top-left, in the frame's format, in the session's render context,
 * run to its end.
 */
static int
i915_bootfb_copy(
	struct i915_device *device,
	void *session,
	void *object,
	const struct gpu_display_present *request,
	uint32_t copy_width,
	uint32_t copy_height)
{
	struct i915_bootfb *bootfb;
	struct i915_session *owner;
	struct i915_gem_object *storage;
	struct i915_gfx_surface src;
	struct i915_gfx_surface dst;
	struct i915_gfx_rect rect;
	int error;

	bootfb = &i915_bootfb_display;
	owner = session;
	storage = object;

	/* A session without an executor, or a frame not in its address space, cannot be copied. */
	if (owner->vk == NULL)
		return EINVAL;
	if (storage->va == 0U)
		return EINVAL;

	/* Makes the staging area addressable in the session's space. */
	error = i915_bootfb_map(device, owner->vm);
	if (error != 0)
		return error;

	/* The frame as the GPU sees it, in the order it was presented. */
	src.va = storage->va + request->offset;
	src.width = request->width;
	src.height = request->height;
	src.pitch = request->stride;
	src.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (request->format == GPU_PIXEL_BGRA8888)
		src.format = VK_FORMAT_B8G8R8A8_UNORM;
	src.tiled = 0U;

	/* The staging area: the copied extent in the same format, so the bytes are copied as they are. */
	dst.va = bootfb->map_va;
	dst.width = copy_width;
	dst.height = copy_height;
	dst.pitch = bootfb->layout.staging_stride_bytes;
	dst.format = src.format;
	dst.tiled = 0U;

	/* The frame's top-left part the framebuffer has room for, unscaled. */
	rect.x = 0;
	rect.y = 0;
	rect.w = copy_width;
	rect.h = copy_height;

	/* Copies it and waits until the copy has run to its end. */
	error = drv_i915_gfx_rect(owner->vk, &dst, &rect, &src, &rect, NULL, 0);
	if (error != 0)
		return error;

	/*
	 * Drops any line of the rows a prefetch may have brought into the
	 * CPU's caches before the GPU's uncached writes reached memory.
	 */
	drv_i915_gt_clflush(bootfb->staging_cpu, (size_t)bootfb->layout.staging_stride_bytes * copy_height);

	/* Succeeded: the staging area holds the frame. */
	return 0;
}

/* Copies the staged rows on to the framebuffer, in its order. */
static void
i915_bootfb_show(
	const struct gpu_display_present *request,
	uint32_t copy_width,
	uint32_t copy_height)
{
	struct i915_bootfb *bootfb;
	const uint32_t *source;
	volatile uint32_t *destination;
	uint32_t row;
	int swap;

	bootfb = &i915_bootfb_display;

	/* Whether the framebuffer's order is the frame's. */
	swap = drv_i915_bootfb_swap(&bootfb->layout, request->format);

	/* Every row, top to bottom. */
	for (row = 0U; row < copy_height; row++) {
		source = (const uint32_t *)(const void *)(bootfb->staging_cpu + (size_t)row * bootfb->layout.staging_stride_bytes);
		destination = (volatile uint32_t *)(volatile void *)(bootfb->fb_cpu + (size_t)row * bootfb->layout.fb_stride_bytes);
		drv_i915_bootfb_copy_row(destination, source, copy_width, swap);
	}
}

/* Clears every visible row of the framebuffer to black, for a new lease. */
static void
i915_bootfb_clear(void)
{
	struct i915_bootfb *bootfb;
	volatile uint32_t *destination;
	uint32_t row;
	uint32_t column;

	bootfb = &i915_bootfb_display;

	/* Every pixel of every row. */
	for (row = 0U; row < bootfb->layout.height; row++) {
		destination = (volatile uint32_t *)(volatile void *)(bootfb->fb_cpu + (size_t)row * bootfb->layout.fb_stride_bytes);
		for (column = 0U; column < bootfb->layout.width; column++)
			destination[column] = 0U;
	}
}

/*
 * Sums the copy times and logs them: the first frame alone, then every
 * I915_BOOTFB_TIMING_FRAMES frames the mean of those frames, in
 * microseconds.
 */
static void
i915_bootfb_time(
	uint64_t gpu_ns,
	uint64_t cpu_ns,
	uint32_t width,
	uint32_t height)
{
	struct i915_bootfb *bootfb;

	bootfb = &i915_bootfb_display;

	/* The first frame tells how long one full copy takes. */
	if (bootfb->frames == 1U) {
		kern_logf("i915: bootfb: frame 1 %ux%u gpu_copy_us=%llu cpu_copy_us=%llu\n",
		    width,
		    height,
		    (unsigned long long)(gpu_ns / I915_BOOTFB_NS_PER_US),
		    (unsigned long long)(cpu_ns / I915_BOOTFB_NS_PER_US));
		return;
	}

	/* The later frames are summed. */
	bootfb->gpu_ns += gpu_ns;
	bootfb->cpu_ns += cpu_ns;
	bootfb->timed++;

	/* Not yet a whole period. */
	if (bootfb->timed < I915_BOOTFB_TIMING_FRAMES)
		return;

	/* The period's means, and a new period. */
	kern_logf("i915: bootfb: %llu frames shown, the last %u at gpu_copy_us=%llu cpu_copy_us=%llu each\n",
	    (unsigned long long)bootfb->frames,
	    bootfb->timed,
	    (unsigned long long)(bootfb->gpu_ns / bootfb->timed / I915_BOOTFB_NS_PER_US),
	    (unsigned long long)(bootfb->cpu_ns / bootfb->timed / I915_BOOTFB_NS_PER_US));
	bootfb->gpu_ns = 0U;
	bootfb->cpu_ns = 0U;
	bootfb->timed = 0U;
}

/*
 * Maps the whole staging area uncached into a presenting session's address
 * space.
 *
 * One address space holds it at a time, from the first presentation of a
 * lease to its end: the same one again is a no-op, another is refused with
 * EBUSY.  Returns 0, or the address space's error.
 */
static int
i915_bootfb_map(
	struct i915_device *device,
	struct i915_ppgtt *vm)
{
	struct i915_bootfb *bootfb;
	uint64_t va;
	unsigned pages;
	int error;

	bootfb = &i915_bootfb_display;
	pages = (unsigned)(bootfb->layout.staging_bytes / I915_BOOTFB_PAGE_BYTES);

	/* The area is already in this address space. */
	if (bootfb->map_vm == vm)
		return 0;

	/* One presenting address space at a time. */
	if (bootfb->map_vm != NULL)
		return EBUSY;

	/* Maps the run under the device mutex, which guards the address spaces. */
	mutex_lock(&device->mutex);

	/* Reserves a range as large as the area; ranges are never reused. */
	error = drv_i915_ppgtt_va_alloc(vm, bootfb->layout.staging_bytes, &va);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		return error;
	}

	/* Maps every page uncached, so the GPU's writes reach memory. */
	error = drv_i915_ppgtt_insert_uncached(vm, va, (uint64_t)bootfb->run.paddr, pages);
	if (error != 0) {
		drv_i915_ppgtt_clear(vm, va, pages);
		mutex_unlock(&device->mutex);
		return error;
	}

	/* The address space holds the area from here on. */
	bootfb->map_vm = vm;
	bootfb->map_va = va;

	mutex_unlock(&device->mutex);

	/* Succeeded: the GPU can write the staging area. */
	return 0;
}

/*
 * Unmaps the staging area from the address space that holds it.
 *
 * XXX: the session address space never reuses a range, so the range is only
 * cleared, not given back, and no TLB invalidation is needed for a range
 * nothing names.
 */
static void
i915_bootfb_unmap(
	struct i915_device *device)
{
	struct i915_bootfb *bootfb;
	unsigned pages;

	bootfb = &i915_bootfb_display;
	pages = (unsigned)(bootfb->layout.staging_bytes / I915_BOOTFB_PAGE_BYTES);

	/* No address space holds the area. */
	if (bootfb->map_vm == NULL)
		return;

	/* Points the range back at scratch under the device mutex. */
	mutex_lock(&device->mutex);

	drv_i915_ppgtt_clear(bootfb->map_vm, bootfb->map_va, pages);

	mutex_unlock(&device->mutex);

	/* No address space holds the area from here on. */
	bootfb->map_vm = NULL;
	bootfb->map_va = 0U;
}

/*
 * Ends the current lease: the staging area's mapping goes first, and the
 * text console gets the screen back.  The caller holds the lease mutex.
 */
static void
i915_bootfb_end_lease(
	struct i915_device *device)
{
	struct i915_resident_display *rd;
	int error;

	rd = &device->display->rd;

	/* The plane gets the firmware's surface back; one that does not come back ends the flips. */
	if (i915_bootfb_display.flip) {
		error = drv_i915_xd9_flip_release(device);
		if (error != 0)
			i915_bootfb_display.flip = 0;
	}

	/* The GPU no longer names the staging area through the owner's space. */
	i915_bootfb_unmap(device);

	/* The display shows no frame of a lease any more. */
	rd->active = 0;

	kern_logf("i915: bootfb: lease %llu released after %llu frame(s) (%llu shown in all); the text console resumes\n",
	    (unsigned long long)rd->lease,
	    (unsigned long long)rd->sequence,
	    (unsigned long long)i915_bootfb_display.frames);

	/* The display is free. */
	rd->owner = NULL;
	rd->lease = 0U;

	/* The text console draws its retained screen again. */
	kern_text_resume();
}

/*
 * Brings the copies back after the flips failed: the firmware's surface
 * is written back, the node's step falls to copy, and the framebuffer is
 * cleared for the copied frames.  The caller holds the lease mutex.
 */
static void
i915_bootfb_flip_failed(
	struct i915_device *device)
{
	/* The flips end for the rest of the boot. */
	drv_i915_xd9_flip_stop(device);
	i915_bootfb_display.flip = 0;

	/* The copied frames start on a cleared framebuffer. */
	i915_bootfb_clear();
}
