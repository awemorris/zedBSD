/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The flip display of a display 9 node.
 *
 * Display 9 (Gen9 and Gen9.5) keeps the mode the firmware
 * set: nothing here enables a pipe, a port, a clock or a power well.  At
 * the binding of the boot framebuffer display (bootfb.c) the firmware's
 * display registers are read without writing any of them; when exactly one
 * pipe is lit and its first plane scans out a linear XRGB8888 surface with
 * no rotation, offset, asynchronous flip, PSR or DC state in the way, two
 * buffers of that plane's extent are made in the GGTT's display window.
 *
 * A presentation then waits until the flip before it has latched (the
 * plane's live surface is that flip's buffer), has the GPU copy the frame
 * into the other buffer, and writes that buffer's address into the plane's
 * surface register.  The write arms the plane's double-buffered registers,
 * which take effect at the start of the next vertical blank, so a frame is
 * never shown while it is being drawn and every frame is shown for at
 * least one refresh.  The plane's flip-done interrupt wakes the waiter;
 * the waiter also reads the live surface every millisecond, so a missing
 * interrupt makes the waits coarser but never wrong.
 *
 * The end of a lease points the plane back at the firmware's surface, so
 * the text console is on the screen again.  A flip that does not latch
 * within I915_XD9_FLIP_LATCH_MS, a fault of the plane reading one of the
 * driver's buffers, or buffers the presenting session's space cannot map
 * give the flips up for the rest of the boot: the firmware's surface is
 * written back, the node's step falls to copy (xd9-step.h), and
 * bootfb.c copies every frame into the firmware's framebuffer as it did
 * before.
 *
 * Every function but the interrupt hook and the resume's check (which
 * only reads registers, with the worker parked) runs under the display's
 * lease mutex (bootfb.c), or before any session exists (the start).
 *
 * XXX: one device; the plane's control, stride and watermarks are the
 * firmware's, so only a mode the firmware set is shown.
 */

#include "internal.h"
#include "xd9-step.h"
#include "xd9-flip.h"
#include "xd9-outputs.h"
#include "xd-io.h"
#include "xd-pipe.h"
#include "xd-takeover.h"
#include "scanout.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../ggtt.h"
#include "../memory.h"
#include "../mmio.h"
#include "../perf.h"
#include "../ppgtt.h"
#include "../session.h"
#include "../sync.h"
#include "../vk/vk-blit.h"
#include "../intel/display-regs-xd9.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The longest sleep for the flip-done interrupt before the surface is read again, in milliseconds (a tick at least). */
#define I915_XD9_FLIP_POLL_MS 1U

/* How many frames one timing line of the log sums up. */
#define I915_XD9_FLIP_TIMING_FRAMES	600U

/* Nanoseconds in a microsecond, for the timing lines. */
#define I915_XD9_FLIP_NS_PER_US		1000U

/* The bytes of a GGTT page, and the multiple of pages the display window is claimed in. */
#define I915_XD9_FLIP_PAGE_BYTES	4096U
#define I915_XD9_FLIP_WINDOW_UNIT	32U

/*
 * The flip display of the node.
 *
 * Made by the start before any session exists and kept until the display
 * stops.  From then on its fields change only under the display's lease
 * mutex, except the interrupt count and the completion, which the
 * interrupt handler touches.
 */
struct i915_xd9_flip {
	/* Nonzero while the flips are used: the readout allowed them and both buffers are pinned. */
	int ready;

	/* What the firmware's display is, and the plane's raw stride the firmware set. */
	struct i915_xd9_fw_readout readout;
	uint32_t firmware_stride;

	/* The device's registers, their windows for the waits and for the interrupt handler, and GT memory. */
	struct i915_mmio *mmio;
	struct i915_xd_io io;
	struct i915_xd_io irq_io;
	struct i915_gt_mem *gm;

	/* The two buffers; the plane shows the firmware's surface, the armed buffer, or the other one. */
	struct i915_scanout buffer[2];
	int made[2];

	/* The address space that maps both buffers, and where; NULL while none does. */
	struct i915_ppgtt *map_vm;
	uint64_t map_va[2];

	/* The buffer the next frame is drawn into, and the surface last written (0: none since the claim). */
	unsigned back;
	uint32_t armed;

	/* The tick the newest flip was seen latched at. */
	uint64_t latched_tick;

	/* Nonzero while the plane's flip-done interrupt is enabled; the handler acknowledges it only then. */
	volatile int irq_on;

	/* Completed by the flip-done interrupt; a waiter sleeps on it a millisecond at a time. */
	struct i915_completion done;

	/*
	 * The plane's faults the interrupt handler saw since the lease's first
	 * flip: a buffer of the driver's the display engine could not read.  A
	 * latch of one of the driver's buffers that finds one ends the flips.
	 * The handler adds to it; the claim and the first flip empty it.
	 */
	volatile uint32_t faults;

	/*
	 * Why the flips failed, for the stop's line, or NULL while they have
	 * not: set by the waits and the presentation when the flips themselves
	 * (not the frame or the GPU) went wrong, so the caller gives them up.
	 */
	const char *failure;

	/* Flip-done interrupts seen, flips armed, and the frames' copy and wait times since the last timing line. */
	volatile uint64_t irq_count;
	uint64_t flips;
	uint64_t gpu_ns;
	uint64_t wait_ns;
	uint32_t timed;
};

/*
 * The flip display of the one device.
 *
 * Zero means no flips: the start fills it when the readout allows them,
 * and the stop or the display's end empties it again.
 */
static struct i915_xd9_flip i915_xd9_flip_state;

static int i915_xd9_flip_make_buffers(struct i915_xd9_flip *flip);
static void i915_xd9_flip_free_buffers(struct i915_xd9_flip *flip);
static int i915_xd9_flip_map(struct i915_device *device, struct i915_ppgtt *vm);
static void i915_xd9_flip_unmap(struct i915_device *device);
static int i915_xd9_flip_latch(struct i915_xd9_flip *flip, uint32_t surf);
static void i915_xd9_flip_arm(struct i915_xd9_flip *flip, uint32_t surf, uint32_t stride);
static int i915_xd9_flip_restore(struct i915_xd9_flip *flip);
static void i915_xd9_flip_irq_enable(struct i915_xd9_flip *flip);
static void i915_xd9_flip_irq_disable(struct i915_xd9_flip *flip);
static void i915_xd9_flip_time(struct i915_xd9_flip *flip, uint64_t gpu_ns, uint64_t wait_ns, uint32_t width, uint32_t height);

/*
 * Reports the display window pages two buffers of the given size need.
 *
 * Each buffer takes its pages, a guard on each side and up to an
 * alignment's worth of skipped pages; the sum is rounded up to the
 * window's unit.
 */
unsigned
drv_i915_xd9_flip_window_pages(
	uint32_t buffer_bytes)
{
	unsigned buffer_pages;
	unsigned pages;

	/* One buffer's pages, guards and alignment slack. */
	buffer_pages = (unsigned)((buffer_bytes + I915_XD9_FLIP_PAGE_BYTES - 1U) / I915_XD9_FLIP_PAGE_BYTES);
	pages = buffer_pages + 2U * I915_XD9_FLIP_GUARD_PAGES + I915_XD9_FLIP_ALIGN_PAGES;

	/* Two buffers, in whole units of the window's bitmap. */
	pages = 2U * pages;
	pages = (pages + I915_XD9_FLIP_WINDOW_UNIT - 1U) / I915_XD9_FLIP_WINDOW_UNIT * I915_XD9_FLIP_WINDOW_UNIT;

	/* Succeeded: the window holds both buffers wherever their alignment falls. */
	return pages;
}

/*
 * Reads the firmware's display and makes the two buffers.
 *
 * fb_physical is the loader's framebuffer, compared with the plane's
 * surface for the log only.  Returns 0 when the flips are used, ENOTSUP
 * when a failure gave the flips up, EINVAL when the
 * firmware's display cannot be flipped, or the window's or the buffers'
 * error.  A failure is logged as the flip step's
 * (drv_i915_xd9_step_fail()) and leaves the copy display; a window too
 * small for the external displays gives up only the mode set.
 */
int
drv_i915_xd9_flip_start(
	struct i915_device *device,
	uint64_t fb_physical)
{
	struct i915_xd9_flip *flip;
	struct i915_xd9_fw_registers registers;
	struct i915_gt *gt;
	const char *fb_match;
	unsigned pages;
	unsigned output_pages;
	int level;
	int error;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;
	gt = &device->gt;
	kern_memset(flip, 0, sizeof(*flip));

	/* Only a display 9 node's firmware display is read. */
	if (gt->graphics_ver != 9U)
		return ENOTSUP;

	/* A failure before may have given the flips up (bootfb.c logs the step taken). */
	level = drv_i915_xd9_step();
	if (level < I915_XD9_STEP_FLIP)
		return ENOTSUP;

	/* Reads what the firmware left, without writing a register. */
	drv_i915_xd_io_init(&flip->io, &gt->mmio, NULL);
	kern_memset(&registers, 0, sizeof(registers));
	drv_i915_xd9_fw_read(&flip->io, &registers);

	/* Decides whether the plane can be flipped. */
	error = drv_i915_xd9_fw_decode(&registers, &flip->readout);

	/* Says whether the loader's framebuffer is the plane's surface (it is not in a virtual machine). */
	fb_match = "other";
	if (fb_physical >= gt->gmadr_base && fb_physical - gt->gmadr_base == (uint64_t)flip->readout.surf)
		fb_match = "same";

	/* What was read, in the log. */
	drv_i915_xd9_fw_log(&registers, &flip->readout, fb_match);

	/* Reports a display the flips cannot stand in for. */
	if (error != 0) {
		drv_i915_xd9_step_fail(I915_XD9_STEP_FLIP, flip->readout.reason, 0);
		kern_memset(flip, 0, sizeof(*flip));
		return error;
	}

	/* The device's registers and GT memory the flips use. */
	flip->mmio = &gt->mmio;
	drv_i915_xd_io_init(&flip->irq_io, &gt->mmio, NULL);
	flip->gm = &gt->mem;
	flip->firmware_stride = registers.plane_stride[flip->readout.pipe];

	/* The display window's pages for the panel's two buffers. */
	pages = drv_i915_xd9_flip_window_pages(flip->readout.stride_bytes * flip->readout.height);

	/*
	 * With the mode set, the window also holds two buffers of the largest
	 * mode for each external output.  A window that large that cannot be
	 * claimed gives the mode set up, and the panel's alone is claimed.
	 */
	error = ENOSPC;
	if (level >= I915_XD9_STEP_MODESET) {
		output_pages = I915_XD9_OUTPUTS * drv_i915_xd9_flip_window_pages(I915_XD9_OUTPUT_MAX_WIDTH * I915_XD9_OUTPUT_MAX_HEIGHT * 4U);
		error = drv_i915_gt_display_window_init(flip->gm, pages + output_pages);
		if (error != 0 && error != EBUSY)
			drv_i915_xd9_step_fail(I915_XD9_STEP_MODESET, "the display window for the external displays could not be claimed", error);
	}

	/* The panel's window, unless the larger one was claimed; one claimed before is used as it is. */
	if (error != 0 && error != EBUSY)
		error = drv_i915_gt_display_window_init(flip->gm, pages);
	if (error != 0 && error != EBUSY) {
		drv_i915_xd9_step_fail(I915_XD9_STEP_FLIP, "the display window could not be claimed", error);
		kern_memset(flip, 0, sizeof(*flip));
		return error;
	}

	/* Makes and pins the two buffers. */
	error = i915_xd9_flip_make_buffers(flip);
	if (error != 0) {
		drv_i915_xd9_step_fail(I915_XD9_STEP_FLIP, "the buffers could not be made", error);
		i915_xd9_flip_free_buffers(flip);
		kern_memset(flip, 0, sizeof(*flip));
		return error;
	}

	/* The waiter's completion, which the flip-done interrupt signals. */
	drv_i915_completion_init(&flip->done, "i915 xd9 flip");

	/* The flips are used from the first presentation of a lease on. */
	flip->ready = 1;

	/* Says where the flips go. */
	kern_logf("i915: xd9-flip: flip on pipe %c, buffers at 0x%08x and 0x%08x (GGTT), the firmware's surface 0x%08x is restored on release\n",
	    'A' + (int)flip->readout.pipe,
	    (uint32_t)flip->buffer[0].surf,
	    (uint32_t)flip->buffer[1].surf,
	    flip->readout.surf);

	/* Succeeded: the node's desktop is shown by flips. */
	return 0;
}

/*
 * Reports the extent and the refresh the flips show: the firmware's
 * plane's.
 */
void
drv_i915_xd9_flip_extent(
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz)
{
	struct i915_xd9_flip *flip;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* The plane's extent and the timing's refresh. */
	*width = flip->readout.width;
	*height = flip->readout.height;
	*refresh_millihz = flip->readout.refresh_millihz;
}

/*
 * Describes the flipped panel for the external outputs (xd9-outputs.c):
 * its pipe, DDI and DPLL, its plane's width, and its line total and
 * pixel clock, which its watermarks are computed from.  Returns 0, or
 * ENOENT when the flips are not used or the pixel clock was not measured.
 */
int
drv_i915_xd9_flip_panel(
	struct i915_xd9_panel *panel)
{
	struct i915_xd9_flip *flip;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Only a flipped panel whose timing was read. */
	if (!flip->ready || !flip->readout.refresh_measured || flip->readout.pixel_khz == 0U)
		return ENOENT;

	/* Succeeded: the panel as its registers say. */
	kern_memset(panel, 0, sizeof(*panel));
	panel->pipe = flip->readout.pipe;
	panel->ddi = flip->readout.port;
	panel->dpll = flip->readout.dpll;
	panel->hdisplay = flip->readout.width;
	panel->htotal = flip->readout.htotal;
	panel->pixel_khz = flip->readout.pixel_khz;
	return 0;
}

/*
 * Readies the buffers and the flip interrupt for a new lease.
 *
 * Both buffers are cleared to black, so a frame smaller than the plane
 * leaves black around it, and the plane keeps showing the firmware's
 * surface until the lease's first presentation.
 */
void
drv_i915_xd9_flip_claim(
	struct i915_device *device)
{
	struct i915_xd9_flip *flip;
	unsigned index;

	UNUSED_PARAMETER(device);

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Without the flips there is nothing to ready. */
	if (!flip->ready)
		return;

	/* Clears both buffers and makes the black visible to the display engine. */
	for (index = 0U; index < 2U; index++) {
		kern_memset(flip->buffer[index].cpu, 0, flip->buffer[index].size);
		drv_i915_scanout_publish(&flip->buffer[index]);
	}

	/* The first frame goes into buffer 0, with no flip armed before it. */
	flip->back = 0U;
	flip->armed = 0U;

	/* Lets the plane's flip-done interrupt wake the waits. */
	i915_xd9_flip_irq_enable(flip);
}

/*
 * Draws a frame into the buffer the plane does not show and flips to it.
 *
 * The flip armed by the previous presentation must latch first, so the
 * buffer drawn into is shown by nothing.  The flip armed here latches at
 * the next vertical blank; the next presentation, or a wait, waits for
 * it.  Returns 0, EINVAL for a session without an executor or a frame not
 * in its address space, the copy's error, or the error of the flips
 * themselves: ETIMEDOUT when the previous flip did not latch, EIO when the
 * plane faulted, or the mapping's error.  After one of the last kind
 * drv_i915_xd9_flip_failed() reports nonzero and the caller gives the
 * flips up and copies the frame instead.
 */
int
drv_i915_xd9_flip_present(
	struct i915_device *device,
	void *session,
	void *object,
	const struct gpu_display_present *request)
{
	struct i915_xd9_flip *flip;
	struct i915_session *owner;
	struct i915_gem_object *storage;
	struct i915_scanout *back;
	struct i915_gfx_surface src;
	struct i915_gfx_surface dst;
	struct i915_gfx_rect rect;
	uint64_t started;
	uint64_t waited;
	uint64_t copied;
	int error;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;
	owner = session;
	storage = object;

	/* Without the flips, or without an executor or a frame in the session's space, nothing can be flipped. */
	if (!flip->ready)
		return EINVAL;
	if (owner->vk == NULL)
		return EINVAL;
	if (storage->va == 0U)
		return EINVAL;
	if (request->width == 0U || request->height == 0U)
		return EINVAL;

	/* Makes both buffers addressable in the session's space; buffers no space can name end the flips. */
	error = i915_xd9_flip_map(device, owner->vm);
	if (error != 0) {
		flip->failure = "the buffers could not be mapped for the GPU";
		return error;
	}

	/* Waits until the plane shows the flip armed before, so the other buffer is free. */
	started = drv_i915_perf_now();
	if (flip->armed != 0U) {
		error = i915_xd9_flip_latch(flip, flip->armed);
		if (error != 0)
			return error;
	}

	/* When the wait ended. */
	waited = drv_i915_perf_now();

	/* The frame as the GPU sees it, in the order it was presented. */
	src.va = storage->va + request->offset;
	src.width = request->width;
	src.height = request->height;
	src.pitch = request->stride;
	src.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (request->format == GPU_PIXEL_BGRA8888)
		src.format = VK_FORMAT_B8G8R8A8_UNORM;
	src.tiled = 0U;

	/* The buffer the plane does not show, in the plane's byte order. */
	back = &flip->buffer[flip->back];
	dst.va = flip->map_va[flip->back];
	dst.width = back->width;
	dst.height = back->height;
	dst.pitch = back->pitch;
	dst.format = VK_FORMAT_B8G8R8A8_UNORM;
	if (flip->readout.rgbx)
		dst.format = VK_FORMAT_R8G8B8A8_UNORM;
	dst.tiled = 0U;

	/* The frame's top-left part the plane has room for, unscaled. */
	rect.x = 0;
	rect.y = 0;
	rect.w = request->width;
	if (rect.w > back->width)
		rect.w = back->width;
	rect.h = request->height;
	if (rect.h > back->height)
		rect.h = back->height;

	/* Copies it and waits until the copy has reached memory. */
	error = drv_i915_gfx_rect(owner->vk, &dst, &rect, &src, &rect, NULL, 0);
	if (error != 0)
		return error;

	/* When the copy ended. */
	copied = drv_i915_perf_now();

	/*
	 * The lease's first flip leaves the firmware's surface: a fault seen
	 * while the plane read that surface is not one of the driver's
	 * buffers', so the faults are counted from here on.
	 */
	if (flip->armed == 0U)
		flip->faults = 0U;

	/* Points the plane at the buffer (its pin keeps the address within 32 bits); it latches at the next vertical blank. */
	i915_xd9_flip_arm(flip, (uint32_t)back->surf, back->stride_units);
	flip->armed = (uint32_t)back->surf;
	flip->back ^= 1U;
	flip->flips++;

	/* How long the wait and the copy took, in the log now and then. */
	i915_xd9_flip_time(flip, copied - waited, waited - started, rect.w, rect.h);

	/* Succeeded: the frame's flip is armed. */
	return 0;
}

/*
 * Waits until the newest flip has latched and reports the tick it was
 * seen latched at.
 *
 * Returns 0 (also when no flip is armed: the last latch is reported),
 * ETIMEDOUT when the flip did not latch, or EIO when the plane faulted;
 * either is the flips' own failure (drv_i915_xd9_flip_failed()).
 */
int
drv_i915_xd9_flip_wait(
	uint64_t *latched_tick)
{
	struct i915_xd9_flip *flip;
	int error;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Waits for the armed flip, if any. */
	if (flip->ready && flip->armed != 0U) {
		error = i915_xd9_flip_latch(flip, flip->armed);
		if (error != 0)
			return error;
	}

	/* Succeeded: the tick the newest flip latched at. */
	*latched_tick = flip->latched_tick;
	return 0;
}

/*
 * Reports nonzero when the error a presentation or a wait just returned
 * was the flips' own (a flip that did not latch, a plane fault, buffers
 * that could not be mapped), which the caller answers by giving the flips
 * up; zero for an error of the frame or of the GPU's copy.
 */
int
drv_i915_xd9_flip_failed(void)
{
	/* A reason was recorded by the presentation or the wait that failed. */
	if (i915_xd9_flip_state.failure != NULL)
		return 1;

	/* Succeeded: the flips themselves did not fail. */
	return 0;
}

/*
 * Checks after a sleep in D0 that the flipped panel's power well,
 * transcoder and plane still run, and logs what it finds.
 *
 * Nothing is written: a panel whose pipe the sleep turned off cannot be
 * lit again without a mode set of the panel, which this driver does not
 * make, so the line says so for the photograph of the failure.
 */
void
drv_i915_xd9_flip_resume(
	struct i915_device *device)
{
	struct i915_xd9_flip *flip;
	uint32_t power;
	uint32_t transconf;
	uint32_t control;
	int runs;

	UNUSED_PARAMETER(device);

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Only a panel the flips took over was read before the sleep. */
	if (!flip->ready)
		return;

	/* The power well, the transcoder and the plane the panel needs. */
	runs = drv_i915_xd9_fw_runs(&flip->io, &flip->readout, &power, &transconf, &control);
	/* A power well, transcoder or plane the sleep turned off leaves the panel dark. */
	if (!runs) {
		kern_logf("i915: xd9-flip: resume failed (pipe %c is off after the sleep: PWR_WELL_CTL2=0x%08x TRANSCONF=0x%08x PLANE_CTL=0x%08x); the panel stays dark until a restart\n",
		    'A' + (int)flip->readout.pipe,
		    power,
		    transconf,
		    control);
		return;
	}

	/* Succeeded: the panel runs as before the sleep. */
	kern_logf("i915: xd9-flip: pipe %c runs after the sleep\n", 'A' + (int)flip->readout.pipe);
}

/*
 * Gives the plane back the firmware's surface at the end of a lease.
 *
 * The buffers stay pinned for the next lease; their mapping in the
 * session's space goes.  Returns 0, or ETIMEDOUT when the surface did not
 * come back: that is logged and the flips are given up.
 */
int
drv_i915_xd9_flip_release(
	struct i915_device *device)
{
	struct i915_xd9_flip *flip;
	int error;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Without the flips the plane never left the firmware's surface. */
	if (!flip->ready)
		return 0;

	/* Shows the firmware's surface again, once it is on the plane. */
	error = i915_xd9_flip_restore(flip);

	/* The waits need the interrupt no longer, and the session's space no longer names the buffers. */
	i915_xd9_flip_irq_disable(flip);
	i915_xd9_flip_unmap(device);

	/* A plane that kept a buffer of ours ends the flips; the buffers are kept. */
	if (error != 0) {
		kern_logf("i915: xd9-flip: the firmware's surface 0x%08x did not come back at the end of the lease\n", flip->readout.surf);
		drv_i915_xd9_step_fail(I915_XD9_STEP_FLIP, "the firmware's surface did not come back", error);
		flip->ready = 0;
		return error;
	}

	/* Says how many flips the lease made. */
	kern_logf("i915: xd9-flip: %llu flip(s), %llu flip-done interrupt(s); the firmware's surface is back\n",
	    (unsigned long long)flip->flips,
	    (unsigned long long)flip->irq_count);

	/* Succeeded: the firmware's surface is on the screen. */
	return 0;
}

/*
 * Gives the flips up for the rest of the boot after they failed (a flip
 * that did not latch, a plane fault, buffers that could not be mapped).
 *
 * The firmware's surface is written back, the interrupt is disabled and
 * the mapping goes; the buffers are kept, since the plane may still read
 * one of them.  The node's step falls to copy with the failure's line.
 * The caller holds the lease mutex.
 */
void
drv_i915_xd9_flip_stop(
	struct i915_device *device)
{
	struct i915_xd9_flip *flip;
	const char *reason;
	uint32_t armed;
	uint32_t live;
	uint32_t faults;
	int error;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Without the flips there is nothing to give up. */
	if (!flip->ready)
		return;

	/* Why the flips end, and what the plane showed, before the restore changes them. */
	reason = flip->failure;
	if (reason == NULL)
		reason = "a flip failed";
	armed = flip->armed;
	faults = flip->faults;
	live = drv_i915_xd9_plane_surf_live(&flip->io, flip->readout.pipe);

	/* Writes the firmware's surface back and gives it the same time to latch. */
	error = i915_xd9_flip_restore(flip);

	/* No more waits, and no more mapping in the session's space. */
	i915_xd9_flip_irq_disable(flip);
	i915_xd9_flip_unmap(device);
	flip->ready = 0;

	/* What the plane showed, for the photograph of a failure. */
	kern_logf("i915: xd9-flip: flip to 0x%08x: live 0x%08x, faults 0x%x; the firmware's surface is %s\n",
	    armed,
	    live,
	    faults,
	    (error == 0) ? "back" : "NOT back");

	/* Says why the flips end: the copies into the firmware's framebuffer from now on. */
	drv_i915_xd9_step_fail(I915_XD9_STEP_FLIP, reason, 0);
}

/*
 * Frees the buffers once the plane is shown not to read them.
 *
 * A buffer the plane still shows, or may show, is abandoned instead: its
 * pages and GGTT entries are kept for ever.
 */
void
drv_i915_xd9_flip_fini(
	struct i915_device *device)
{
	struct i915_xd9_flip *flip;
	uint32_t live;
	unsigned index;

	UNUSED_PARAMETER(device);

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Nothing was made. */
	if (flip->mmio == NULL)
		return;

	/* The interrupt goes first. */
	i915_xd9_flip_irq_disable(flip);

	/* Keeps a buffer the plane shows; frees the others. */
	live = drv_i915_xd9_plane_surf_live(&flip->io, flip->readout.pipe);
	for (index = 0U; index < 2U; index++) {
		if (flip->made[index] && flip->buffer[index].surf == (uint64_t)live) {
			kern_logf("i915: xd9-flip: XXX buffer %u is still on the plane at stop; it is kept\n", index);
			drv_i915_scanout_abandon(&flip->buffer[index]);
			flip->made[index] = 0;
		}
	}

	/* The buffers the plane does not show go. */
	i915_xd9_flip_free_buffers(flip);

	/* Forgets the flips. */
	kern_memset(flip, 0, sizeof(*flip));
}

/*
 * Acknowledges the flip's pipe interrupt and wakes the waiter.
 *
 * Runs in the interrupt handler with the master interrupt control off.
 * Only the flip pipe's sources are read; the handler does nothing while
 * the interrupt is not enabled.
 */
void
drv_i915_xd9_flip_irq(
	uint32_t master_ctl)
{
	struct i915_xd9_flip *flip;
	uint32_t iir;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* Only an enabled flip interrupt is served. */
	if (!flip->irq_on)
		return;

	/* What the flip pipe reports, acknowledged. */
	iir = drv_i915_xd9_flip_irq_ack(&flip->irq_io, flip->readout.pipe, master_ctl);
	if (iir == 0U)
		return;

	/* A fault is kept for the waiter, which gives the flips up when it sees one. */
	if ((iir & I915_XD9_DE_PIPE_FAULT) != 0U) {
		flip->faults |= iir & I915_XD9_DE_PIPE_FAULT;
		drv_i915_complete(&flip->done);
	}

	/* A flip latched: the waiter reads the live surface now. */
	if ((iir & I915_XD9_DE_PIPE_FLIP_DONE) != 0U) {
		flip->irq_count++;
		drv_i915_complete(&flip->done);
	}
}

/* Makes and pins the two buffers of the plane's extent; the caller frees what was made on failure. */
static int
i915_xd9_flip_make_buffers(
	struct i915_xd9_flip *flip)
{
	unsigned index;
	int error;

	/* Each buffer: its backing and its GGTT range. */
	for (index = 0U; index < 2U; index++) {
		error = drv_i915_scanout_create(flip->gm, flip->readout.width, flip->readout.height, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &flip->buffer[index]);
		if (error != 0)
			return error;

		/* Made: freed on a failure. */
		flip->made[index] = 1;

		/* Its GGTT range. */
		error = drv_i915_scanout_pin(&flip->buffer[index], "xd9 flip");
		if (error != 0)
			return error;
	}

	/* Succeeded: both buffers have a surface address. */
	return 0;
}

/* Unpins and destroys the buffers made, leaving the abandoned ones alone. */
static void
i915_xd9_flip_free_buffers(
	struct i915_xd9_flip *flip)
{
	unsigned index;
	int error;

	/* Each buffer made: the GGTT range, then the backing. */
	for (index = 0U; index < 2U; index++) {
		if (!flip->made[index])
			continue;

		/* A buffer that was never pinned has no range to give back. */
		if (flip->buffer[index].state >= I915_SCANOUT_PINNED) {
			error = drv_i915_scanout_unpin(&flip->buffer[index]);
			if (error != 0) {
				kern_logf("i915: xd9-flip: XXX buffer %u could not be unpinned: %d; it is kept\n", index, error);
				continue;
			}
		}

		/* The backing. */
		error = drv_i915_scanout_destroy(&flip->buffer[index]);
		if (error != 0)
			kern_logf("i915: xd9-flip: XXX buffer %u could not be destroyed: %d\n", index, error);

		/* Succeeded: the buffer is gone. */
		flip->made[index] = 0;
	}
}

/*
 * Maps both buffers uncached into a presenting session's address space.
 *
 * One address space holds them at a time, from the first presentation of
 * a lease to its end: the same one again is a no-op, another is refused
 * with EBUSY.  Returns 0, or the address space's error.
 */
static int
i915_xd9_flip_map(
	struct i915_device *device,
	struct i915_ppgtt *vm)
{
	struct i915_xd9_flip *flip;
	struct i915_gt_object *object;
	uint64_t va;
	uint64_t dma;
	unsigned index;
	unsigned page;
	int error;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* The buffers are already in this address space. */
	if (flip->map_vm == vm)
		return 0;

	/* One presenting address space at a time. */
	if (flip->map_vm != NULL)
		return EBUSY;

	/* Maps each buffer under the device mutex, which guards the address spaces. */
	mutex_lock(&device->mutex);

	error = 0;
	for (index = 0U; index < 2U && error == 0; index++) {
		object = flip->buffer[index].obj;

		/* Reserves a range as large as the buffer; ranges are never reused. */
		error = drv_i915_ppgtt_va_alloc(vm, (uint64_t)object->pages * I915_XD9_FLIP_PAGE_BYTES, &va);
		if (error != 0)
			break;

		/* Maps every page uncached, in order, so the GPU's writes reach the memory the plane reads. */
		for (page = 0U; error == 0 && page < object->pages; page++) {
			error = drv_i915_gt_object_page_dma(object, page, &dma);
			if (error != 0) {
				error = EIO;
				break;
			}

			/* The page, uncached, in order. */
			error = drv_i915_ppgtt_insert_uncached(vm, va + (uint64_t)page * I915_XD9_FLIP_PAGE_BYTES, dma, 1U);
		}

		/* The buffer's range. */
		flip->map_va[index] = va;
	}

	/* A buffer that could not be mapped is pointed back at scratch with the ones before it. */
	if (error != 0) {
		while (index > 0U) {
			index--;
			if (flip->map_va[index] != 0U)
				drv_i915_ppgtt_clear(vm, flip->map_va[index], flip->buffer[index].obj->pages);
			flip->map_va[index] = 0U;
		}

		mutex_unlock(&device->mutex);
		return error;
	}

	/* The address space holds the buffers from here on. */
	flip->map_vm = vm;

	mutex_unlock(&device->mutex);

	/* Succeeded: the GPU can draw into both buffers. */
	return 0;
}

/*
 * Unmaps both buffers from the address space that holds them.
 *
 * XXX: the session address space never reuses a range, so the ranges are
 * only cleared, not given back.
 */
static void
i915_xd9_flip_unmap(
	struct i915_device *device)
{
	struct i915_xd9_flip *flip;
	unsigned index;

	/* The flips of the one device. */
	flip = &i915_xd9_flip_state;

	/* No address space holds the buffers. */
	if (flip->map_vm == NULL)
		return;

	/* Points both ranges back at scratch under the device mutex. */
	mutex_lock(&device->mutex);

	for (index = 0U; index < 2U; index++) {
		drv_i915_ppgtt_clear(flip->map_vm, flip->map_va[index], flip->buffer[index].obj->pages);
		flip->map_va[index] = 0U;
	}

	mutex_unlock(&device->mutex);

	/* No address space holds the buffers from here on. */
	flip->map_vm = NULL;
}

/*
 * Waits until the plane's live surface is the given one.
 *
 * Sleeps a millisecond at a time; the flip-done interrupt ends a sleep early.
 * Returns 0 with the latch's tick recorded, ETIMEDOUT after
 * I915_XD9_FLIP_LATCH_MS, or EIO when one of the driver's buffers latched
 * but the plane faulted reading it (the screen would show nothing of it);
 * either failure is recorded for the stop's line.
 */
static int
i915_xd9_flip_latch(
	struct i915_xd9_flip *flip,
	uint32_t surf)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t live;
	uint32_t faults;

	/* Reads the live surface until it is the one asked for, or the time is up. */
	deadline = sched_ticks() + kern_ms_to_ticks(I915_XD9_FLIP_LATCH_MS) + 1U;
	for (;;) {
		live = drv_i915_xd9_plane_surf_live(&flip->io, flip->readout.pipe);
		if (live == surf)
			break;

		/* The time is up: the flip did not latch. */
		now = sched_ticks();
		if (now >= deadline) {
			flip->failure = "a flip did not latch within 100 ms";
			return ETIMEDOUT;
		}

		/* Sleeps until the flip-done interrupt, or a millisecond at most. */
		(void)drv_i915_wait_for_completion(&flip->done, now + kern_ms_to_ticks(I915_XD9_FLIP_POLL_MS));
	}

	/* The faults the handler saw, and one it has not served yet. */
	faults = drv_i915_xd9_plane_fault(&flip->io, flip->readout.pipe);
	faults |= flip->faults;

	/* A buffer of the driver's the plane could not read shows nothing; the firmware's surface is the way back. */
	if (faults != 0U && surf != flip->readout.surf) {
		flip->faults = faults;
		flip->failure = "the plane faulted reading the driver's buffer";
		return EIO;
	}

	/* Succeeded: the plane shows the surface from this tick on. */
	flip->latched_tick = sched_ticks();
	return 0;
}

/*
 * Points the plane at a surface: the stride first when it differs from
 * the one the plane has, then the surface, whose write arms both for the
 * next vertical blank.
 */
static void
i915_xd9_flip_arm(
	struct i915_xd9_flip *flip,
	uint32_t surf,
	uint32_t stride)
{
	/* A stale completion would only end the next sleep early; it starts empty for this flip. */
	drv_i915_reinit_completion(&flip->done);

	/* Succeeded: the stride when it changes, and the surface that arms it. */
	drv_i915_xd9_plane_flip(&flip->io, flip->readout.pipe, surf, stride);
}

/*
 * Points the plane back at the firmware's surface and stride, and waits
 * for the latch.
 *
 * Returns 0, or ETIMEDOUT when the plane did not take the firmware's
 * surface back.
 */
static int
i915_xd9_flip_restore(
	struct i915_xd9_flip *flip)
{
	int error;

	/* Nothing of the driver's was ever armed since the claim. */
	if (flip->armed == 0U)
		return 0;

	/* The firmware's surface and stride, armed together. */
	i915_xd9_flip_arm(flip, flip->readout.surf, flip->firmware_stride);

	/* Waits until the plane shows it. */
	error = i915_xd9_flip_latch(flip, flip->readout.surf);
	if (error != 0)
		return error;

	/* No buffer of the driver's is armed or shown. */
	flip->armed = 0U;
	flip->back = 0U;

	/* Succeeded: the firmware's surface is on the screen. */
	return 0;
}

/*
 * Enables the plane's flip-done and fault interrupts on the flip pipe:
 * clears what is pending, enables the sources and unmasks them.  The other
 * display sources stay as the device start left them (masked and
 * disabled).
 */
static void
i915_xd9_flip_irq_enable(
	struct i915_xd9_flip *flip)
{
	/* Already enabled for this lease. */
	if (flip->irq_on)
		return;

	/* A stale flip done or fault cleared, and no fault seen yet in this lease. */
	drv_i915_xd9_flip_irq_clear(&flip->io, flip->readout.pipe);
	flip->faults = 0U;

	/* The handler acknowledges the sources from here on. */
	flip->irq_on = 1;

	/* Succeeded: the sources enabled and unmasked. */
	drv_i915_xd9_flip_irq_unmask(&flip->io, flip->readout.pipe);
}

/* Masks and disables the plane's flip-done and fault interrupts and clears what is pending. */
static void
i915_xd9_flip_irq_disable(
	struct i915_xd9_flip *flip)
{
	/* Not enabled. */
	if (!flip->irq_on)
		return;

	/* The sources masked and disabled, and what is pending cleared. */
	drv_i915_xd9_flip_irq_mask(&flip->io, flip->readout.pipe);
	drv_i915_xd9_flip_irq_clear(&flip->io, flip->readout.pipe);

	/* Succeeded: the handler leaves the source alone from here on. */
	flip->irq_on = 0;
}

/*
 * Sums the copy and wait times and logs them: the first frame alone, then
 * every I915_XD9_FLIP_TIMING_FRAMES frames the means, in microseconds.
 */
static void
i915_xd9_flip_time(
	struct i915_xd9_flip *flip,
	uint64_t gpu_ns,
	uint64_t wait_ns,
	uint32_t width,
	uint32_t height)
{
	/* The first frame tells how long one copy takes. */
	if (flip->flips == 1U) {
		kern_logf("i915: xd9-flip: frame 1 %ux%u gpu_copy_us=%llu flip-done irqs %llu\n",
		    width,
		    height,
		    (unsigned long long)(gpu_ns / I915_XD9_FLIP_NS_PER_US),
		    (unsigned long long)flip->irq_count);
		return;
	}

	/* The later frames are summed. */
	flip->gpu_ns += gpu_ns;
	flip->wait_ns += wait_ns;
	flip->timed++;

	/* Not yet a whole period. */
	if (flip->timed < I915_XD9_FLIP_TIMING_FRAMES)
		return;

	/* The period's means, and a new period. */
	kern_logf("i915: xd9-flip: %llu flips, the last %u at gpu_copy_us=%llu latch_wait_us=%llu each, flip-done irqs %llu\n",
	    (unsigned long long)flip->flips,
	    flip->timed,
	    (unsigned long long)(flip->gpu_ns / flip->timed / I915_XD9_FLIP_NS_PER_US),
	    (unsigned long long)(flip->wait_ns / flip->timed / I915_XD9_FLIP_NS_PER_US),
	    (unsigned long long)flip->irq_count);
	flip->gpu_ns = 0U;
	flip->wait_ns = 0U;
	flip->timed = 0U;
}

