/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The present path of the resident node.
 *
 * The node's display is the eDP panel, with one full-output plane and the
 * panel's own mode.  A session claims the display and gets a lease; its
 * presentations name the lease, so a stale presentation after a release is
 * refused.  Two routes show a frame:
 *
 *   shared   the frame is a blob the rendering connection exported and the
 *            display connection imported: the GPU copies it (scaled by a
 *            whole factor, centred) into the panel's back buffer, then the
 *            panel flips.  No CPU touches the pixels.
 *   copied   the frame is ordinary storage: the CPU copies it the same way.
 *
 * Every presentation is queued to the request worker and waited for.  The
 * first one makes the worker enter the display window: the panel is lit
 * (the resident run of modeset.c) and the worker keeps serving every
 * request from inside the window.  A release does not make it leave: the
 * output holds the last picture of the lease that ended, and the next
 * lease's first frame is drawn into the other buffer and flipped to, so a
 * login or a logout (the greeter's lease, then the session's, then the
 * greeter's again) never goes dark (ws075-p016).  A hold that no lease
 * ends within I915_PRESENT_HOLD_MS, or the shutdown, makes the worker
 * leave the window, and the panel is stopped through the reference's stop
 * path.
 *
 * FIFO (ws075-p019): a presentation returns once its flip is armed, so the
 * presenting thread draws its next frame while the flip waits for its
 * vblank; the next presentation first waits for that flip to latch, then
 * draws into the buffer the panel stopped showing.  Every frame is shown
 * for at least one vblank and no copy ever meets the scanout, so nothing
 * tears.  The display wait operation waits for the latch of the newest
 * flip.  A kernel built with I915_PRESENT_NO_VSYNC set does not wait at
 * all instead: a frame that comes before the armed flip has latched is
 * drawn into the buffer that flip shows (the newest frame wins, as in a
 * mailbox), and the copy may meet the latch, which tears.
 *
 * XXX: one output, one plane, one lease at a time; a requested mode smaller
 * than the panel is shown scaled and centred, the display is never
 * re-timed.  No hot-plug and no topology events.
 */

#include "internal.h"
#include "display.h"
#include "output.h"
#include "modeset.h"
#include "present.h"
#include "scanout.h"
#include "control.h"
#include "head.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../memory.h"
#include "../session.h"
#include "../worker.h"
#include "../render/blit.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The FNV-1a offset basis and prime the first-frame check hashes with. */
#define I915_PRESENT_FNV_BASIS		2166136261U
#define I915_PRESENT_FNV_PRIME		16777619U

/*
 * How long the output holds the last picture of an ended lease for the
 * next one before it is stopped.  It covers the hand-over of a login or a
 * logout (the greeter or the session exits, the next one starts and
 * presents), and bounds how long a picture of nobody stays on the screen.
 */
#define I915_PRESENT_HOLD_MS		10000U

/* How long the shutdown waits for the worker to stop a held output, in steps of 10 ms. */
#define I915_PRESENT_SHUTDOWN_STEPS	200U

/* How many of the first GPU-copied frames the check reads back. */
#define I915_PRESENT_CHECKED_FRAMES	2U

/*
 * Whether a presentation returns without waiting for its flip (the build
 * option I915_PRESENT_NO_VSYNC, default 0: FIFO, one flip per vblank and
 * a wait for each).
 */
#ifndef I915_PRESENT_NO_VSYNC
#define I915_PRESENT_NO_VSYNC		0
#endif

/*
 * The GPU copy of one shared frame into a panel buffer.
 *
 * Built on the asking thread (the kernels and the session's objects are made
 * there) and read by the worker, which builds and runs the copy; it lives on
 * the asking thread's stack while the presentation is queued.
 */
struct i915_present_blit {
	/* The session's executor, whose state and batch objects the copy uses. */
	struct i915_render_session *vk;

	/* The frame as the GPU sees it. */
	struct i915_gfx_surface src;

	/* The frame as the CPU sees it (for the check of the first frames). */
	const uint32_t *cpu;
};

static int i915_present_release_locked(struct i915_device *device);
static int i915_present_blit_build(void *ctx, uint64_t dst_va, uint32_t width, uint32_t height, uint32_t pitch, uint64_t *batch_va);
static int i915_present_shared(struct i915_device *device, void *session, void *object, struct gpu_display_present *request, int head);
static int i915_present_head(struct i915_device *device, void *session, void *object, struct gpu_display_present *request);
static int i915_present_head_wait(struct i915_device *device, void *session, struct gpu_display_wait *request);
static int i915_present_window_retry(struct i915_display *display, int error, int *spared);
static int i915_present_window_serve(void *ctx);
static void i915_present_check_frame(struct i915_display *display, const struct i915_worker_present *frame, struct i915_scanout *back, unsigned index);
static struct i915_scanout *i915_present_target(struct i915_display *display, int *flip);
static int i915_present_flip(struct i915_display *display, int publish);
static void i915_present_hold_end(struct i915_display *display);
static void i915_present_clear_stale(struct i915_display *display, struct i915_scanout *back, uint32_t width, uint32_t height, uint32_t scale);
static int i915_present_latch(struct i915_device *device);

/*
 * Shows a CPU frame on the panel.
 *
 * Queued to the worker and waited for: the worker lights the panel for the
 * first frame, then copies the frame into the back buffer and flips.
 */
int
drv_i915_present(
	struct i915_device *device,
	const void *pixels,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	int bgra)
{
	struct i915_worker_present item;
	int error;

	/* Describes the frame. */
	kern_memset(&item, 0, sizeof(item));
	item.pixels = pixels;
	item.width = width;
	item.height = height;
	item.stride = stride;
	item.bgra = bgra;

	/* Hands it to the worker and waits. */
	error = drv_i915_worker_sync_display(device, I915_WORKER_SYNC_PRESENT, &item);
	if (error != 0)
		return error;

	/* Succeeded: the frame is on the panel. */
	return 0;
}

/*
 * Gives the panel back: the worker leaves the display window and the panel
 * is stopped through the reference's stop path.
 */
int
drv_i915_present_release(
	struct i915_device *device)
{
	struct i915_worker_present item;
	int error;

	/* A release carries no frame. */
	kern_memset(&item, 0, sizeof(item));

	/* Hands it to the worker and waits. */
	error = drv_i915_worker_sync_display(device, I915_WORKER_SYNC_RELEASE, &item);
	if (error != 0)
		return error;

	/* Succeeded: the panel is stopped, or holds the last picture for the next lease. */
	return 0;
}

/*
 * Ends a hold of the last picture before the machine goes down.
 *
 * Runs from the PCI shutdown.  Every later hold is refused, and a hold in
 * progress makes the worker leave the window: the output is stopped, as it
 * was before a hold existed, instead of showing a picture of nobody while
 * the machine halts.  Waits a bounded time for the stop.  A lease that is
 * still held keeps its output.
 */
void
drv_i915_present_shutdown(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned long irq;
	unsigned step;
	int holding;

	display = device->display;

	/* A device without a display holds nothing. */
	if (display == NULL)
		return;

	/*
	 * hold_ended refuses every later hold and ends the current one; the
	 * worker sees it in its next look at the queue.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->window.hold_ended = 1;
	holding = display->window.holding;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Nothing is held: nothing to stop. */
	if (!holding)
		return;

	/* Wakes the worker to see the end of the hold. */
	drv_i915_worker_wake(device);

	/* Waits, in bounded steps, until the worker has stopped the output. */
	for (step = 0U; step < I915_PRESENT_SHUTDOWN_STEPS; step++) {
		/* Gives the worker 10 ms to leave the window and stop the output. */
		kern_usleep_range(10000U, 11000U);

		/* Reads whether it still holds the picture. */
		irq = spin_lock_irqsave(&device->irq_lock);

		holding = display->window.holding;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* The worker has left the window and the output is stopped. */
		if (!holding)
			break;
	}

	/* Says whether the output was stopped before the machine goes down. */
	if (holding) {
		kern_logf("i915: resident display: XXX the held picture was not stopped within %u ms of the shutdown\n",
		    I915_PRESENT_SHUTDOWN_STEPS * 10U);
	} else {
		kern_logf("i915: resident display: the held picture was stopped for the shutdown\n");
	}
}

/*
 * Ends the hold of the last picture now, for a claim that moves the
 * output (ws113-p011a): the worker leaves the display window and stops the
 * output that showed it.  Unlike the shutdown, later holds are not
 * refused.  Waits a bounded time.  Returns 0 once nothing is held (also
 * when nothing was), or ETIMEDOUT.
 */
int
drv_i915_present_cut(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned long irq;
	unsigned step;
	int holding;

	/* A device without a display holds nothing. */
	display = device->display;
	if (display == NULL)
		return 0;

	/* hold_cut ends the hold in progress once; the worker sees it in its next look at the queue. */
	irq = spin_lock_irqsave(&device->irq_lock);

	holding = display->window.holding;
	if (holding)
		display->window.hold_cut = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Nothing is held: nothing to stop. */
	if (!holding)
		return 0;

	/* Wakes the worker to see the end of the hold. */
	drv_i915_worker_wake(device);

	/* Waits, in bounded steps, until the worker has stopped the output. */
	for (step = 0U; step < I915_PRESENT_SHUTDOWN_STEPS; step++) {
		/* Gives the worker 10 ms to leave the window and stop the output. */
		kern_usleep_range(10000U, 11000U);

		/* Reads whether it still holds the picture. */
		irq = spin_lock_irqsave(&device->irq_lock);

		holding = display->window.holding;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* The worker has left the window and the output is stopped. */
		if (!holding)
			break;
	}

	/* Not stopped in time. */
	if (holding)
		return ETIMEDOUT;

	/* Succeeded: nothing is held. */
	return 0;
}

/*
 * Shows a frame of the lease holder on the panel (the display present
 * operation).
 *
 * A shared blob is copied by the GPU, ordinary storage by the CPU.  Returns
 * 0 with the completed sequence in the request, EINVAL for a session that
 * does not hold the lease or a frame larger than the panel, ESTALE for
 * another generation, ENXIO when the panel is unknown, or the presentation's
 * error.
 */
int
drv_i915_present_display_present(
	void *device,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct i915_device *owner_device;
	struct i915_display *display;
	struct i915_resident_display *rd;
	struct i915_gem_object *storage;
	const uint8_t *pixels;
	const char *route;
	const char *order;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint32_t display_id;
	uint64_t generation;
	uint64_t start;
	int connected;
	int owns_head;
	int error;

	owner_device = device;
	display = owner_device->display;
	rd = &display->rd;
	storage = object;

	/* The second output's lease presents on the second output (ws113-p011). */
	owns_head = drv_i915_head_owns(display, session, request->lease);
	if (owns_head) {
		error = i915_present_head(owner_device, session, object, request);
		if (error != 0)
			return error;

		/* Succeeded: the frame is on the second output. */
		return 0;
	}

	/* The whole presentation is timed, the wait for the lease included. */
	start = drv_i915_perf_now();

	/* Checks the lease and the frame under the lease mutex. */
	drv_i915_present_lease_init(display);
	mutex_lock(&rd->mutex);

	/* Only the lease holder presents. */
	if (rd->owner != session || rd->lease != request->lease) {
		mutex_unlock(&rd->mutex);
		return EINVAL;
	}

	/* The resident output of this generation, still connected (ws113-p011a: a connector's identity). */
	(void)drv_i915_display_resident_identity(display, &display_id, &generation, &connected);
	if (request->generation != generation) {
		mutex_unlock(&rd->mutex);
		return ESTALE;
	}

	/* Its connector unplugged: the frame cannot be shown (ws113-p011a, the kind's own detection does not matter). */
	if (!connected) {
		mutex_unlock(&rd->mutex);
		return ENXIO;
	}

	/* The frame must fit the panel. */
	error = drv_i915_display_panel(display, &width, &height, &refresh);
	if (error == 0) {
		if (request->width > width || request->height > height)
			error = EINVAL;
	}

	if (error != 0) {
		mutex_unlock(&rd->mutex);
		return error;
	}

	/* FIFO: the flip the previous presentation armed latches before this frame takes the buffer it leaves. */
	error = i915_present_latch(owner_device);
	if (error != 0) {
		mutex_unlock(&rd->mutex);
		return error;
	}

	/* The shared route: the GPU copies the imported blob into the panel's back buffer. */
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U) {
		error = i915_present_shared(owner_device, session, object, request, 0);
	} else {
		/* The copied route: the core checked the extent against the storage, which is ordinary managed RAM. */
		pixels = (const uint8_t *)kern_pmem_to_kernel(storage->run.paddr) + request->offset;
		error = drv_i915_present(owner_device, pixels, request->width, request->height, request->stride, request->format == GPU_PIXEL_BGRA8888);
	}

	/* A completed presentation: the panel shows this node's frames, and the sequence advances. */
	if (error == 0) {
		rd->active = 1;
		rd->sequence++;
		rd->present_tick = sched_ticks();
		request->sequence = rd->sequence;

		/* The first frame names the route it took. */
		if (rd->sequence == 1U) {
			route = "copied, CPU copy";
			if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U)
				route = "shared, GPU copy";

			order = "RGBA";
			if (request->format == GPU_PIXEL_BGRA8888)
				order = "BGRA";

			kern_logf("i915: resident display: first frame %ux%u (stride %u; %s; %s) shown on the %ux%u panel\n",
			    request->width,
			    request->height,
			    request->stride,
			    route,
			    order,
			    width,
			    height);
		}
	}

	mutex_unlock(&rd->mutex);

	/* Reports a failed presentation. */
	if (error != 0)
		return error;

	/* Counts the presentation's time and logs the timing once a window is over. */
	drv_i915_perf_add(&owner_device->perf, I915_PERF_PRESENT, start);
	drv_i915_perf_report(&owner_device->perf, 0);

	/* Succeeded: the frame is on the panel. */
	return 0;
}

/*
 * Reports the completed sequence of the lease (the display wait operation).
 *
 * The newest presentation completes when its flip latches: the wait waits
 * for that (FIFO, ws075-p019).  Returns 0, EINVAL for a session that does
 * not hold the lease or a sequence not presented yet, EAGAIN before the
 * first presentation, or EIO when the flip did not latch.
 */
int
drv_i915_present_display_wait(
	void *device,
	void *session,
	struct gpu_display_wait *request)
{
	struct i915_device *owner_device;
	struct i915_display *display;
	struct i915_resident_display *rd;
	uint32_t display_id;
	uint64_t generation;
	int connected;
	int owns_head;
	int error;

	owner_device = device;
	display = owner_device->display;
	rd = &display->rd;

	/* The second output's lease waits on the second output (ws113-p011). */
	owns_head = drv_i915_head_owns(display, session, request->lease);
	if (owns_head) {
		error = i915_present_head_wait(owner_device, session, request);
		if (error != 0)
			return error;

		/* Succeeded: the sequence has completed. */
		return 0;
	}

	/* Reads the lease under its mutex. */
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

	/* The newest presentation's flip latches; the tick it did so at is its completion. */
	error = i915_present_latch(owner_device);
	if (error != 0) {
		mutex_unlock(&rd->mutex);
		return error;
	}

	/* The newest sequence, the tick it completed at, and the generation. */
	request->completed_sequence = rd->sequence;
	request->present_time_ns = rd->present_tick * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	(void)drv_i915_display_resident_identity(display, &display_id, &generation, &connected);
	request->generation = generation;

	mutex_unlock(&rd->mutex);

	/* Succeeded: the sequence has completed. */
	return 0;
}

/*
 * Ends the lease of its holder (the display release operation).
 *
 * If the panel shows this node's frames it is stopped first.  Returns 0,
 * EINVAL for a session that does not hold the lease, or the stop's error.
 */
int
drv_i915_present_display_release(
	void *device,
	void *session,
	const struct gpu_display_release *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;
	int owns_head;
	int error;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* The second output's lease ends the second output (ws113-p011): it goes dark at once. */
	owns_head = drv_i915_head_owns(owner_device->display, session, request->lease);
	if (owns_head) {
		error = drv_i915_head_release(owner_device, session, request->lease);
		if (error != 0)
			return error;

		/* Succeeded: the lease is free. */
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

	error = i915_present_release_locked(owner_device);

	mutex_unlock(&rd->mutex);

	/* Reports a stop that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the lease is free. */
	return 0;
}

/*
 * Prepares the display lease once: its mutex, and the first lease number.
 */
void
drv_i915_present_lease_init(
	struct i915_display *display)
{
	struct i915_resident_display *rd;

	rd = &display->rd;

	/* The lease is prepared already. */
	if (rd->inited)
		return;

	/* The mutex, and lease numbers from 1. */
	(void)mutex_init(&rd->mutex, LOCK_RANK_DEVICE, "i915-resident-display");
	rd->next_lease = 1U;
	rd->inited = 1;
}

/*
 * Ends the lease a closing session still holds; the panel is stopped
 * first.
 */
void
drv_i915_present_lease_close(
	struct i915_device *device,
	void *session)
{
	struct i915_resident_display *rd;

	/* A device without a display has no lease. */
	if (device->display == NULL)
		return;

	/* The second output's lease ends first, as the compositor would end it (ws113-p011). */
	drv_i915_head_lease_close(device, session);

	rd = &device->display->rd;

	/* A lease that was never prepared was never held. */
	if (!rd->inited)
		return;

	/* Ends the session's lease under the lease mutex. */
	mutex_lock(&rd->mutex);

	if (rd->owner == session) {
		kern_logf("i915: resident display: the lease owner closed without releasing it\n");
		(void)i915_present_release_locked(device);
	}

	mutex_unlock(&rd->mutex);
}

/*
 * Reports whether a presentation should enter the display window: the node
 * has a panel, the firmware's output has not failed, and no moved output
 * failed in the current lease.
 */
int
drv_i915_present_window_ready(
	struct i915_device *device)
{
	struct i915_display *display;

	display = device->display;

	/* A node without a panel shows nothing. */
	if (display == NULL)
		return 0;
	if (display->rctx.lcd == NULL)
		return 0;

	/* The firmware's output that failed once is not tried again. */
	if (display->window.display_failed)
		return 0;

	/* A moved output that failed fails the rest of its lease; the next lease tries again (ws113-p011a). */
	if (display->window.lease_failed)
		return 0;

	/* The window can be entered. */
	return 1;
}

/*
 * Reports whether the current lease's moved output failed (ws113-p011a):
 * its presentations fail until the next claim, as for an output that is
 * gone (BUG-266).
 */
int
drv_i915_present_lease_failed(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned long irq;
	int failed;

	/* A node without a display has no lease to fail. */
	display = device->display;
	if (display == NULL)
		return 0;

	/* The mark the fail-back sets under the lock the worker's readers share. */
	irq = spin_lock_irqsave(&device->irq_lock);

	failed = display->window.lease_failed;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: whether the lease's moved output failed. */
	return failed;
}

/*
 * Runs the display window on the worker.
 *
 * Lights the panel (the resident run); the window serves every request,
 * across the holds between leases, until a hold is over or a stop is asked
 * for, and the reference's stop path follows.  A moved output that did not
 * come up, or did not stop cleanly, gives the firmware's output back and
 * fails the rest of its lease (ws113-p011a); the next lease lights an
 * output again.  XXX: the firmware's output that did not come up, or did
 * not stop cleanly, is not tried again: presentation fails from here on
 * and the node keeps serving.
 */
void
drv_i915_present_window(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned long irq;
	int after_resume;
	int moved;
	int retry;
	int spared;
	int error;

	display = device->display;

	/*
	 * Lights the panel; the window serves until a hold is over or a stop.
	 * An external DP link that did not train is lowered and lit again
	 * (EAGAIN, ws051-p004b); every retry lowers the port's link, so the
	 * retries end.  A window left so that a second output's first frame
	 * finds the resident output lit for two pipes is lit again at once,
	 * and so is a run for two pipes that failed cleanly, for one pipe with
	 * the second output latched limited (ws113-p011).  moved tells whether
	 * the run lights an output a claim moved to, not the firmware's
	 * (ws113-p011a).
	 */
	moved = drv_i915_display_output_moved(display);
	spared = 0;
	error = drv_i915_lcd_kernel_resident_run(display, display->rctx.lcd, i915_present_window_serve, display);
	retry = i915_present_window_retry(display, error, &spared);
	while (retry) {
		error = drv_i915_lcd_kernel_resident_run(display, display->rctx.lcd, i915_present_window_serve, display);
		retry = i915_present_window_retry(display, error, &spared);
	}

	/*
	 * The first entry after a sleep whose HDMI display did not come up:
	 * the display was unplugged during the sleep, and the built-in panel is
	 * tried in its place once (ws052-p009, decided 2026-10-05).  A panel
	 * that does not come up either ends as any failed run does.
	 */
	after_resume = display->window.after_resume;
	display->window.after_resume = 0;
	if (error != 0 && after_resume && display->output.kind != I915_OUTPUT_KIND_PANEL) {
		kern_logf("i915: resident display: the %s display did not come back after the sleep (%d); trying the built-in panel\n", drv_i915_display_output_name(display), error);
		drv_i915_display_output_panel(display, &display->output);
		moved = drv_i915_display_output_moved(display);
		error = drv_i915_lcd_kernel_resident_run(display, display->rctx.lcd, i915_present_window_serve, display);
	}

	/* A moved output that failed gives the firmware's output back (ws113-p011a), and the rest of its lease fails. */
	if (error != 0 && moved)
		drv_i915_display_output_fail_back(device);

	/* A moved output whose last hold ended without a lease gives the firmware's back (ws113-p011a), before the hold is over. */
	drv_i915_display_output_back(device);

	/*
	 * The output is stopped: no picture is held any more (a shutdown waiting
	 * for the stop sees it here), and the buffers of the next lighting start
	 * black.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->window.holding = 0;
	display->window.hold_cut = 0;
	display->window.stale_buffers = 0U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/*
	 * A failed run of the firmware's output makes every later presentation
	 * fail; the window is not entered for it again.  A run for two pipes
	 * that failed but gave everything back is spared (ws113-p011, review
	 * F9): the second output is latched limited and the next lighting is
	 * for one pipe.
	 */
	if (error != 0 && !moved && !spared && !display->window.display_failed) {
		display->window.display_failed = 1;
		kern_logf("i915: resident display: the firmware's output did not come up (or did not stop cleanly); presentation fails from now on\n");
	}
}

/*
 * Starts holding the last picture for the next lease, for a release that
 * reached the worker inside the window.
 *
 * The caller holds the device IRQ lock.  Returns 1 when the hold started,
 * or 0 when the shutdown has ended holding: the worker then leaves the
 * window and the output is stopped.
 */
int
drv_i915_present_hold_start(
	struct i915_device *device)
{
	struct i915_display *display;

	display = device->display;

	/* The shutdown stops the output instead. */
	if (display->window.hold_ended)
		return 0;

	/*
	 * The hold runs from now until the next lease's first frame, or until
	 * it runs out and the output is stopped.  Both buffers show the ended
	 * lease's pictures until a frame covers them.
	 */
	display->window.holding = 1;
	display->window.hold_since = sched_ticks();
	display->window.hold_until = display->window.hold_since + KERN_MS_TO_TICKS(I915_PRESENT_HOLD_MS);
	display->window.stale_buffers = 3U;

	/* Succeeded: the output holds the last picture. */
	return 1;
}

/*
 * Reports whether a hold is over: it ran out, or the shutdown ended it.
 *
 * The caller holds the device IRQ lock.  Returns 1 when the worker is to
 * leave the window and stop the output, 0 otherwise (no hold, or a hold
 * that goes on).
 */
int
drv_i915_present_hold_over(
	struct i915_device *device)
{
	struct i915_display *display;
	uint64_t now;

	display = device->display;

	/* Nothing is held. */
	if (!display->window.holding)
		return 0;

	/* The shutdown ends the hold at once, and so does a claim that moves the output (ws113-p011a). */
	if (display->window.hold_ended)
		return 1;
	if (display->window.hold_cut)
		return 1;

	/* The hold ran out: no lease came to take the output. */
	now = sched_ticks();
	if (now >= display->window.hold_until)
		return 1;

	/* The hold goes on. */
	return 0;
}

/*
 * Prepares a hold after its start: the ended lease's address space gives
 * up the panel buffers before its release completes and the address space
 * may go.  The caller does not hold the device IRQ lock.
 */
void
drv_i915_present_hold_prepare(
	struct i915_device *device)
{
	struct i915_display *display;

	display = device->display;

	/* The next lease maps the buffers into its own address space. */
	drv_i915_scanout_unmap_panel(display);

	/* Says which buffer the output holds, and for how long at most. */
	kern_logf("i915: resident display: lease released; holding the last picture (buffer %c) for the next lease for up to %u ms\n",
	    (char)('A' + display->resident_front),
	    I915_PRESENT_HOLD_MS);
}

/*
 * Asks for the resident output to be lit again because its external DP
 * link must be trained again (ws051-p005b, the Linux
 * intel_dp_retrain_link()): the worker leaves the window at the next frame
 * and the window's end lights the output again, its link trained afresh,
 * its buffers and picture kept.  Runs from the hotplug path.
 */
void
drv_i915_present_retrain_request(
	struct i915_display *display)
{
	struct i915_device *device;
	unsigned long irq;

	device = display->device;

	/* The request, taken by the worker under the same lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->window.retrain = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Asks for the window to be left and the resident output lit again for
 * two pipes, when a frame of the second output finds the resident output
 * lit for one (ws113-p011, the 2026-10-07 user decision: the first output
 * is lit again when the second is added).  The resident buffers, and
 * their picture, are kept across the new lighting.  Runs on the worker
 * inside the window; the caller holds the device IRQ lock.  Returns 1 when
 * the window is to be left, 0 otherwise.
 */
int
drv_i915_present_relight_begin(
	struct i915_device *device)
{
	struct i915_display *display;
	int needs;

	display = device->display;

	/* A link that must be trained again (ws051-p005b): the output is lit again, its buffers kept. */
	if (display->window.retrain) {
		display->window.retrain = 0;
		display->window.relight = 1;
		display->window.keep_buffers = 1;
		kern_logf("i915: resident display: the window is left to train the link again\n");
		return 1;
	}

	/* Only a second output that the run left no room for. */
	needs = drv_i915_head_needs_relight(device);
	if (!needs)
		return 0;

	/*
	 * relight makes the window's end light the resident output again at
	 * once; keep_buffers keeps its buffers for it.
	 */
	display->window.relight = 1;
	display->window.keep_buffers = 1;

	/* Succeeded: the window is to be left. */
	return 1;
}

/*
 * Shows one CPU frame from inside the window.
 *
 * XXX: a CPU copy, nearest-neighbour scaled by the largest whole factor
 * that fits the panel, centred; the rest of the buffer stays black.
 * Returns 0, EIO when the panel is not up or the flip failed, or EINVAL
 * for a frame larger than the panel.
 */
int
drv_i915_present_frame(
	struct i915_device *device,
	const struct i915_worker_present *frame)
{
	struct i915_display *display;
	struct i915_scanout *back;
	const uint32_t *src;
	uint32_t *row;
	uint32_t scale;
	uint32_t x0;
	uint32_t y0;
	uint32_t x;
	uint32_t y;
	uint32_t pixel;
	int flip;
	int flip_error;

	display = device->display;

	/* The buffer the frame goes into, and whether the panel must flip to it. */
	back = i915_present_target(display, &flip);
	if (back == NULL)
		return EIO;

	/* The largest whole factor that fits both directions. */
	scale = back->width / frame->width;
	if (back->height / frame->height < scale)
		scale = back->height / frame->height;

	if (scale == 0U) {
		kern_logf("i915: resident display: XXX a %ux%u frame is larger than the %ux%u panel\n",
		    frame->width,
		    frame->height,
		    back->width,
		    back->height);
		return EINVAL;
	}

	/* The first frame of a lease after a hold ends the hold; a buffer of the ended lease is cleared if the frame leaves it showing. */
	i915_present_hold_end(display);
	i915_present_clear_stale(display, back, frame->width, frame->height, scale);

	/* Centres the scaled frame. */
	x0 = (back->width - frame->width * scale) / 2U;
	y0 = (back->height - frame->height * scale) / 2U;

	/* Copies every scaled row. */
	for (y = 0U; y < frame->height * scale; y++) {
		src = (const uint32_t *)(const void *)(frame->pixels + (uint64_t)(y / scale) * frame->stride);
		row = back->cpu + (uint64_t)(y0 + y) * (back->pitch / 4U) + x0;

		/* Copies every scaled pixel of the row. */
		for (x = 0U; x < frame->width * scale; x++) {
			pixel = src[x / scale];

			/* The scanout is XRGB8888 (B G R X in memory): RGBA swaps R and B, BGRA is the same order. */
			if (!frame->bgra)
				pixel = ((pixel & 0xffU) << 16) | (pixel & 0xff00U) | ((pixel >> 16) & 0xffU);

			row[x] = pixel & 0x00ffffffU;
		}
	}

	/* Shows the buffer: the CPU wrote it, so its cache lines are published first. */
	if (flip) {
		flip_error = i915_present_flip(display, 1);
		if (flip_error != 0)
			return EIO;
	} else {
		drv_i915_scanout_publish(back);
	}

	/* Succeeded: one more presentation completed. */
	display->window.presents++;
	return 0;
}

/*
 * Shows one GPU-copied frame from inside the window.
 *
 * Maps both panel buffers into the presenting session's address space
 * (once), builds the copy into the back buffer, runs it in the session's
 * context and flips.  The first frames are read back for the log.
 * Returns 0, EIO when the panel is not up or the flip failed, or the
 * mapping's, the builder's or the batch's error.
 */
int
drv_i915_present_blob_frame(
	struct i915_device *device,
	const struct i915_worker_present *frame)
{
	struct i915_display *display;
	struct i915_scanout *back;
	struct i915_scanout *first;
	uint64_t batch_va;
	uint64_t start;
	uint32_t scale;
	unsigned index;
	int flip;
	int error;
	int flip_error;

	display = device->display;

	/* The buffer the frame goes into, and whether the panel must flip to it. */
	back = i915_present_target(display, &flip);
	if (back == NULL)
		return EIO;

	/* The first frame of a lease after a hold ends the hold. */
	i915_present_hold_end(display);

	/* Makes both buffers addressable in the session's space. */
	error = drv_i915_scanout_map_panel(display, frame->vm);
	if (error != 0)
		return error;

	/* The largest whole factor that fits both directions, as the copy scales. */
	scale = 0U;
	if (frame->width != 0U && frame->height != 0U) {
		scale = back->width / frame->width;
		if (back->height / frame->height < scale)
			scale = back->height / frame->height;
	}

	/* A buffer of the ended lease is cleared if the frame leaves it showing. */
	i915_present_clear_stale(display, back, frame->width, frame->height, scale);

	/* Which of the two the back buffer is. */
	first = drv_i915_lcd_resident_buffer(display, 0U);
	index = 1U;
	if (back == first)
		index = 0U;

	/* Builds the copy and runs it in the session's context, timing both. */
	start = drv_i915_perf_now();
	batch_va = 0U;
	error = frame->build(frame->build_ctx, display->window.map_va[index], back->width, back->height, back->pitch, &batch_va);
	if (error == 0)
		error = drv_i915_worker_run_batch(device, frame->context, batch_va);
	drv_i915_perf_add(&device->perf, I915_PERF_PRESENT_COPY, start);

	/* The first frames, as the CPU reads them after the GPU (source and panel buffer). */
	if (error == 0 && display->window.presents < I915_PRESENT_CHECKED_FRAMES)
		i915_present_check_frame(display, frame, back, index);

	/* Reports a copy that failed. */
	if (error != 0) {
		kern_logf("i915: resident display: the GPU copy into the panel buffer failed: %d\n", error);
		return error;
	}

	/*
	 * Shows the buffer.  Only the GPU wrote it, through an uncached
	 * mapping, so no CPU cache line needs publishing.
	 */
	if (flip) {
		flip_error = i915_present_flip(display, 0);
		if (flip_error != 0)
			return EIO;
	}

	/* Succeeded: one more presentation completed. */
	display->window.presents++;
	return 0;
}

/*
 * Reports how many presentations completed.
 */
unsigned
drv_i915_present_count(
	struct i915_device *device)
{
	/* A node without a display presented nothing. */
	if (device->display == NULL)
		return 0U;

	/* Reports the count. */
	return device->display->window.presents;
}

/*
 * Flips the panel to the back buffer of the resident run.
 *
 * What the CPU or the GPU wrote is made visible first.  Returns 0 once the
 * new buffer is displayed and the old one free, EINVAL when the panel is
 * not up, or the flip's error (EIO when the flip did not complete).
 */
int
drv_i915_lcd_resident_flip(
	struct i915_display *display)
{
	int error;

	/* Publishes the back buffer and flips to it, waiting for the completion. */
	error = i915_present_flip(display, 1);
	if (error != 0)
		return error;

	/* Succeeded: the new buffer is displayed. */
	return 0;
}

/*
 * Ends the current lease; if the panel shows this node's frames it is
 * stopped first.  The caller holds the lease mutex.
 */
static int
i915_present_release_locked(
	struct i915_device *device)
{
	struct i915_resident_display *rd;
	const char *stop;
	int error;

	rd = &device->display->rd;

	/* A light the lease switched off comes back first (ws113-p012). */
	drv_i915_display_power_restore_locked(device);

	/* The panel shows this node's frames: the worker stops it. */
	error = 0;
	if (rd->active) {
		error = drv_i915_present_release(device);
		rd->active = 0;
	}

	/* Names how the lease ended. */
	stop = "FAILED";
	if (error == 0)
		stop = "done";

	/* Logs what the timing window holds so far. */
	drv_i915_perf_report(&device->perf, 1);

	kern_logf("i915: resident display: lease %llu released after %llu frame(s) (stop %s)\n",
	    (unsigned long long)rd->lease,
	    (unsigned long long)rd->sequence,
	    stop);

	/* The panel is free. */
	rd->owner = NULL;
	rd->lease = 0U;

	/* Reports a stop that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the lease is ended. */
	return 0;
}

/*
 * Builds the GPU copy of a shared frame into a panel buffer: scaled by the
 * largest whole factor that fits, centred (the render/blit contract).
 */
static int
i915_present_blit_build(
	void *ctx,
	uint64_t dst_va,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint64_t *batch_va)
{
	struct i915_present_blit *blit;
	struct i915_gfx_surface dst;
	struct i915_gfx_rect src_rect;
	struct i915_gfx_rect dst_rect;
	uint32_t scale;
	int error;

	blit = ctx;

	/* The panel buffer: XRGB8888 is B G R X in memory. */
	dst.va = dst_va;
	dst.width = width;
	dst.height = height;
	dst.pitch = pitch;
	dst.format = VK_FORMAT_B8G8R8A8_UNORM;
	dst.tiled = 0U;

	/* The largest whole factor that fits both directions. */
	scale = width / blit->src.width;
	if (height / blit->src.height < scale)
		scale = height / blit->src.height;

	if (scale == 0U)
		return EINVAL;

	/* The whole frame, scaled and centred. */
	src_rect.x = 0;
	src_rect.y = 0;
	src_rect.w = blit->src.width;
	src_rect.h = blit->src.height;
	dst_rect.w = blit->src.width * scale;
	dst_rect.h = blit->src.height * scale;
	dst_rect.x = (int32_t)((width - dst_rect.w) / 2U);
	dst_rect.y = (int32_t)((height - dst_rect.h) / 2U);

	/* Records the rectangle into the session's batch. */
	error = drv_i915_gfx_rect_build(blit->vk, &dst, &dst_rect, &blit->src, &src_rect, NULL, 0, batch_va);
	if (error != 0)
		return error;

	/* Succeeded: the batch writes the frame into the buffer. */
	return 0;
}

/*
 * Presents an imported blob through the GPU copy, on the resident output
 * or on the second output (head, ws113-p011).
 *
 * The copy's kernels and the session's objects are made here, on the
 * asking thread, not on the worker.
 */
static int
i915_present_shared(
	struct i915_device *device,
	void *session,
	void *object,
	struct gpu_display_present *request,
	int head)
{
	struct i915_session *owner;
	struct i915_gem_object *storage;
	struct i915_present_blit blit;
	struct i915_worker_present item;
	int error;

	owner = session;
	storage = object;

	/* The frame as the GPU and the CPU see it. */
	kern_memset(&blit, 0, sizeof(blit));
	blit.vk = owner->vk;
	blit.src.va = 0U;
	if (storage->va != 0U)
		blit.src.va = storage->va + request->offset;

	blit.src.width = request->width;
	blit.src.height = request->height;
	blit.src.pitch = request->stride;
	blit.cpu = (const uint32_t *)(const void *)((const uint8_t *)kern_pmem_to_kernel(storage->run.paddr) + request->offset);
	blit.src.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (request->format == GPU_PIXEL_BGRA8888)
		blit.src.format = VK_FORMAT_B8G8R8A8_UNORM;

	/* A session without an executor, or a blob not in its space, cannot be copied. */
	if (blit.vk == NULL || blit.src.va == 0U)
		return EINVAL;

	/* Makes the copy's kernels and objects. */
	error = drv_i915_gfx_rect_prepare(blit.vk);
	if (error != 0)
		return error;

	/*
	 * The worker maps the panel into the session's space and runs the copy
	 * in its render context, and arms the flip.  The flip latches while the
	 * presenting thread goes on; the next presentation waits for it.
	 */
	kern_memset(&item, 0, sizeof(item));
	item.width = request->width;
	item.height = request->height;
	item.context = &owner->contexts[I915_ENGINE_RCS0];
	item.vm = owner->vm;
	item.build = i915_present_blit_build;
	item.build_ctx = &blit;
	item.head = head;
	error = drv_i915_worker_sync_display(device, I915_WORKER_SYNC_PRESENT_BLOB, &item);
	if (error != 0)
		return error;

	/* Succeeded: the frame's flip is armed. */
	return 0;
}

/*
 * The display window's body: the worker serves everything from inside it
 * until a release; the GPU's mappings of the panel buffers go before the
 * buffers do.
 */
static int
i915_present_window_serve(
	void *ctx)
{
	struct i915_display *display;

	display = ctx;

	/* The worker is inside the window while it serves; the pipe's refresh boundaries can be read from now on. */
	drv_i915_display_refresh_up(display, 1);

	/* A second output kept over the last window's end comes back with its picture (ws113-p011). */
	drv_i915_head_resume(display);

	/* Serves everything until the window is to be left. */
	drv_i915_worker_serve_window(display->device);

	/* A lit second output stops before the resident output does, its picture kept for the next window (ws113-p011); its lease stays. */
	drv_i915_head_stop(display, 1);

	/* The mappings go first; the window is left. */
	drv_i915_scanout_unmap_panel(display);
	drv_i915_display_refresh_up(display, 0);

	/* The window was served. */
	return 0;
}

/* Logs the source and the panel buffer of a GPU-copied frame as the CPU reads them. */
static void
i915_present_check_frame(
	struct i915_display *display,
	const struct i915_worker_present *frame,
	struct i915_scanout *back,
	unsigned index)
{
	const struct i915_present_blit *blit;
	const uint32_t *src;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_pitch;
	uint32_t x;
	uint32_t y;
	uint32_t value;
	uint32_t nonblack_source;
	uint32_t nonblack_panel;
	uint32_t hash_source;
	uint32_t hash_panel;
	uint32_t first_source;

	/* The present path built this copy, so its context is the blit. */
	blit = frame->build_ctx;
	src = blit->cpu;
	source_width = blit->src.width;
	source_height = blit->src.height;
	source_pitch = blit->src.pitch;

	/* Counts and hashes the source's pixels after the GPU. */
	nonblack_source = 0U;
	hash_source = I915_PRESENT_FNV_BASIS;
	first_source = 0U;
	if (src != NULL) {
		drv_i915_gt_clflush(src, (size_t)source_pitch * source_height);
		for (y = 0U; y < source_height; y++) {
			for (x = 0U; x < source_width; x++) {
				value = src[y * (source_pitch / 4U) + x];
				if ((value & 0xffffffU) != 0U)
					nonblack_source++;
				hash_source = (hash_source ^ value) * I915_PRESENT_FNV_PRIME;
			}
		}

		first_source = src[0];
	}

	/* Counts and hashes the panel buffer's pixels. */
	drv_i915_gt_clflush(back->cpu, back->size);
	nonblack_panel = 0U;
	hash_panel = I915_PRESENT_FNV_BASIS;
	for (y = 0U; y < back->height; y++) {
		for (x = 0U; x < back->width; x++) {
			value = back->cpu[y * (back->pitch / 4U) + x];
			if ((value & 0xffffffU) != 0U)
				nonblack_panel++;
			hash_panel = (hash_panel ^ value) * I915_PRESENT_FNV_PRIME;
		}
	}

	kern_logf("i915: resident display: check frame %u: source %ux%u pitch %u non-black %u fnv %08x (px(0,0)=%08x) | panel buffer %c non-black %u of %u fnv %08x (px(center)=%08x)\n",
	    display->window.presents + 1U,
	    source_width,
	    source_height,
	    source_pitch,
	    nonblack_source,
	    hash_source,
	    first_source,
	    (char)('A' + index),
	    nonblack_panel,
	    back->width * back->height,
	    hash_panel,
	    back->cpu[(back->height / 2U) * (back->pitch / 4U) + back->width / 2U]);
}

/*
 * Chooses the resident buffer a frame is drawn into, and whether the panel
 * must then flip to it; NULL when the panel is not up.
 *
 * Normally it is the buffer the panel does not show.  Without the wait for
 * the vblank, a flip may still be armed: the frame then replaces the one
 * that flip shows, in the buffer it latches, and no new flip is needed.
 */
static struct i915_scanout *
i915_present_target(
	struct i915_display *display,
	int *flip)
{
	struct i915_scanout *back;
	int idle;

	/* The panel shows its buffers only while it is up. */
	back = drv_i915_lcd_resident_back(display);
	if (back == NULL)
		return NULL;

	/* An armed flip that has not latched keeps the frame in the buffer it shows next. */
	*flip = 1;
	idle = drv_i915_lcd_modeset_flip_poll(display);
	if (!idle) {
		*flip = 0;
		return &display->resident_buf[display->resident_front];
	}

	/* Nothing armed: the buffer the panel does not show. */
	return back;
}

/*
 * Flips the panel to the back buffer of the resident run, publishing the
 * CPU's writes first when `publish` is set.
 *
 * The flip waits for its completion, or only arms it in a kernel built
 * with I915_PRESENT_NO_VSYNC set; either way the back buffer is the front
 * one afterwards.  Returns 0, EINVAL when the panel is not up, or EIO when
 * the flip failed or did not complete.
 */
static int
i915_present_flip(
	struct i915_display *display,
	int publish)
{
	struct i915_lcd_kernel *k;
	struct i915_scanout *to;
	struct i915_lcd_flip_result result;
	uint64_t start;
	int expected;
	int error;

	k = &display->lk;

	/* Only a panel that is up has a back buffer. */
	if (!display->resident_up)
		return EINVAL;

	/* The back buffer, with what the CPU wrote made visible. */
	to = &display->resident_buf[display->resident_front ^ 1U];
	if (publish) {
		start = drv_i915_perf_now();
		drv_i915_scanout_publish(to);
		drv_i915_perf_add(&display->device->perf, I915_PERF_PRESENT_PUBLISH, start);
	}

	/*
	 * Arms the flip; it latches at the next vblank.  The worker never waits
	 * for it: a presentation that waits (FIFO) does so on the presenting
	 * thread once the worker has handed the item back, so the worker can run
	 * the next frame's rendering meanwhile.
	 */
	kern_memset(&result, 0, sizeof(result));
	start = drv_i915_perf_now();
	expected = I915_LCD_FLIP_ARMED;
	error = drv_i915_lcd_modeset_flip_nowait(display, (uint32_t)to->surf, &result);
	drv_i915_perf_add(&display->device->perf, I915_PERF_PRESENT_FLIP, start);

	/* Says why the flip failed. */
	if (error != 0 || result.result != expected) {
		kern_logf("i915: resident display: flip to 0x%08x failed: rc=%d result=%d event_rc=%d live 0x%08x\n",
		    (uint32_t)to->surf,
		    error,
		    result.result,
		    result.event_rc,
		    result.live_after);
		return EIO;
	}

	/* The back buffer is the front one now. */
	display->resident_front ^= 1U;
	k->flips_done++;

	/* The first flips are logged. */
	if (k->flips_done <= 3U) {
		kern_logf("i915: resident display: flip %u: surf 0x%08x -> 0x%08x | frame %u -> %u\n",
		    k->flips_done,
		    result.old_surf,
		    result.new_surf,
		    result.frame_before,
		    result.frame_after);
	}

	/* Succeeded: the new buffer is displayed, or armed to be at the next vblank. */
	return 0;
}

/* Ends a hold of the last picture at the next lease's first frame, which is a flip and not a modeset. */
static void
i915_present_hold_end(
	struct i915_display *display)
{
	struct i915_device *device;
	unsigned long irq;
	uint64_t held;
	int holding;

	device = display->device;

	/* The hold is over from here: a shutdown no longer waits for a stop. */
	irq = spin_lock_irqsave(&device->irq_lock);

	holding = display->window.holding;
	held = sched_ticks() - display->window.hold_since;
	display->window.holding = 0;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Nothing was held: this is not a lease's first frame after a hold. */
	if (!holding)
		return;

	/* Says how long the output held the ended lease's picture. */
	kern_logf("i915: resident display: the next lease's first frame after holding the last picture for %llu ms (no modeset)\n",
	    (unsigned long long)kern_ticks_to_ms(held));
}

/*
 * Clears a resident buffer that still shows an ended lease's picture when
 * the scaled frame about to be drawn into it does not cover it, so the
 * border around the frame is black as on a fresh lighting.  A frame that
 * covers the buffer replaces the picture by itself.
 */
static void
i915_present_clear_stale(
	struct i915_display *display,
	struct i915_scanout *back,
	uint32_t width,
	uint32_t height,
	uint32_t scale)
{
	unsigned bit;
	int covered;

	/* Which buffer this is. */
	bit = 2U;
	if (back == &display->resident_buf[0])
		bit = 1U;

	/* A buffer drawn by this lease already needs nothing. */
	if ((display->window.stale_buffers & bit) == 0U)
		return;

	/* The buffer is this lease's from here: the frame below replaces or clears the old picture. */
	display->window.stale_buffers &= ~bit;

	/* A frame that fills the buffer leaves nothing of the old picture. */
	covered = 0;
	if (scale != 0U &&
	    width * scale == back->width &&
	    height * scale == back->height)
		covered = 1;

	if (covered)
		return;

	/* Black under the frame, visible to the display and to the GPU's copy. */
	kern_memset(back->cpu, 0, back->size);
	drv_i915_scanout_publish(back);
}

/*
 * Waits for the flip the last presentation armed to latch, so the buffer it
 * leaves is free and the frame it shows has completed (FIFO, ws075-p019).
 * A kernel built with I915_PRESENT_NO_VSYNC set never waits.  The caller
 * holds the lease mutex.  Returns 0, or EIO when the flip did not latch.
 */
static int
i915_present_latch(
	struct i915_device *device)
{
	uint64_t start;
	int error;

	/* No wait without the vsync, or before the panel is up. */
	if (I915_PRESENT_NO_VSYNC)
		return 0;
	if (!device->display->resident_up)
		return 0;

	/*
	 * The vblank the armed flip latches at (none armed returns at once),
	 * timed, on the resident output's screen 0 by name: the worker may
	 * have the second output's screen selected meanwhile (ws113-p011).
	 */
	start = drv_i915_perf_now();
	error = drv_i915_lcd_modeset_flip_wait_screen(device->display, 0U);
	drv_i915_perf_add(&device->perf, I915_PERF_PRESENT_FLIP, start);

	/* Reports a flip that did not latch. */
	if (error != 0)
		return error;

	/* Succeeded: no flip is pending any more. */
	return 0;
}

/*
 * Shows a frame of the second output's lease on it (ws113-p011): the
 * lease, the connector's generation and connection, the frame's size,
 * then the latch of the head's armed flip and the frame queued to the
 * worker, which lights the head first when it is not lit.  A failure of
 * the head is told as the output's loss (ENXIO), never as the device's.
 * Returns 0 with the sequence, EINVAL, ESTALE, ENXIO, or the copy's
 * preparation's error.
 */
static int
i915_present_head(
	struct i915_device *device,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct i915_display *display;
	struct i915_display_head *head;
	struct i915_gem_object *storage;
	struct i915_worker_present item;
	struct i915_hpd_output found;
	const uint8_t *pixels;
	void *resident_owner;
	int error;

	display = device->display;
	head = &display->head;
	storage = object;

	/*
	 * Without the resident output's lease the second output is not shown
	 * (review F10: its frame would light the resident output black, with
	 * no lease to end its window).
	 */
	drv_i915_present_lease_init(display);
	mutex_lock(&display->rd.mutex);

	resident_owner = display->rd.owner;

	mutex_unlock(&display->rd.mutex);

	/* Refused without it. */
	if (resident_owner == NULL)
		return ENXIO;

	/* The lease, under the head's mutex. */
	mutex_lock(&head->mutex);

	if (head->owner != session || head->lease != request->lease) {
		mutex_unlock(&head->mutex);
		return EINVAL;
	}

	/* The connector of this generation, still connected. */
	error = drv_i915_display_output_connector(display, head->connector, &found);
	if (error != 0) {
		mutex_unlock(&head->mutex);
		return ENXIO;
	}

	/* Another generation: the compositor opens the display again. */
	if (request->generation != found.generation) {
		mutex_unlock(&head->mutex);
		return ESTALE;
	}

	/* Unplugged: the output is lost. */
	if (!found.connected) {
		mutex_unlock(&head->mutex);
		return ENXIO;
	}

	/* The frame must fit the head's mode. */
	if (request->width > (uint32_t)head->output.state.mode.hdisplay ||
	    request->height > (uint32_t)head->output.state.mode.vdisplay) {
		mutex_unlock(&head->mutex);
		return EINVAL;
	}

	/* FIFO: the head's armed flip latches before this frame takes the buffer it leaves. */
	if (!I915_PRESENT_NO_VSYNC && head->up) {
		error = drv_i915_lcd_modeset_flip_wait_screen(display, 1U);
		if (error != 0) {
			mutex_unlock(&head->mutex);
			return ENXIO;
		}
	}

	/* The shared route copies the blob by the GPU; the copied route by the CPU. */
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U) {
		error = i915_present_shared(device, session, object, request, 1);
	} else {
		pixels = (const uint8_t *)kern_pmem_to_kernel(storage->run.paddr) + request->offset;
		kern_memset(&item, 0, sizeof(item));
		item.pixels = pixels;
		item.width = request->width;
		item.height = request->height;
		item.stride = request->stride;
		item.bgra = request->format == GPU_PIXEL_BGRA8888;
		item.head = 1;
		error = drv_i915_worker_sync_display(device, I915_WORKER_SYNC_PRESENT, &item);
	}

	/* A head that could not be lit, copied or flipped is the output's loss, not the device's (review F3). */
	if (error == EIO || error == ETIMEDOUT)
		error = ENXIO;

	/* A completed presentation: the sequence advances; the first frame is named. */
	if (error == 0) {
		head->sequence++;
		head->present_tick = sched_ticks();
		request->sequence = head->sequence;
		if (head->sequence == 1U)
			kern_logf("i915: display head: first frame %ux%u shown (connector %u)\n", request->width, request->height, head->connector);
	}

	mutex_unlock(&head->mutex);

	/* Reports a failed presentation. */
	if (error != 0)
		return error;

	/* Succeeded: the frame is on the second output. */
	return 0;
}

/*
 * Reports the completed sequence of the second output's lease (the display
 * wait operation on it, ws113-p011): the head's newest flip latches first.
 * Returns 0, EINVAL, EAGAIN before the first presentation, or ENXIO when
 * the flip did not latch.
 */
static int
i915_present_head_wait(
	struct i915_device *device,
	void *session,
	struct gpu_display_wait *request)
{
	struct i915_display *display;
	struct i915_display_head *head;
	struct i915_hpd_output found;
	int error;

	display = device->display;
	head = &display->head;

	/* The lease, under the head's mutex: only a sequence it presented. */
	mutex_lock(&head->mutex);

	if (head->owner != session ||
	    head->lease != request->lease ||
	    request->sequence > head->sequence) {
		mutex_unlock(&head->mutex);
		return EINVAL;
	}

	/* Nothing has been presented yet. */
	if (head->sequence == 0U) {
		mutex_unlock(&head->mutex);
		return EAGAIN;
	}

	/* The newest flip latches; a head that stopped meanwhile has nothing armed. */
	if (!I915_PRESENT_NO_VSYNC && head->up) {
		error = drv_i915_lcd_modeset_flip_wait_screen(display, 1U);
		if (error != 0) {
			mutex_unlock(&head->mutex);
			return ENXIO;
		}
	}

	/* The newest sequence, the tick it completed at, and the connector's generation. */
	request->completed_sequence = head->sequence;
	request->present_time_ns = head->present_tick * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	request->generation = head->generation;
	error = drv_i915_display_output_connector(display, head->connector, &found);
	if (error == 0)
		request->generation = found.generation;

	mutex_unlock(&head->mutex);

	/* Succeeded: the sequence has completed. */
	return 0;
}

/*
 * Tells whether the resident run is to be tried again at once: an external
 * DP link that did not train (EAGAIN, ws051-p004b), a window left after a
 * clean run so that the resident output is lit again for two pipes
 * (ws113-p011), or a run for two pipes that failed before the display was
 * handed anything, which is tried for one pipe with the second output
 * latched limited.  A run for two pipes that failed after it was lit but
 * gave everything back is not tried again (no lease may be waiting, review
 * F9); spared says that the display is not to be failed for it.  Runs on
 * the worker.
 */
static int
i915_present_window_retry(
	struct i915_display *display,
	int error,
	int *spared)
{
	unsigned i;
	int retained;
	int buffers_free;

	/* A lower DP link is tried. */
	if (error == EAGAIN)
		return 1;

	/* The window was left after a clean run to light the resident output again for two pipes. */
	if (display->window.relight) {
		display->window.relight = 0;
		if (error == 0) {
			kern_logf("i915: resident display: lit again for two pipes, for the second output's first frame\n");
			return 1;
		}
	}

	/* A run for one pipe, or one that came up, is not tried again. */
	if (error == 0 || display->window.run_pipes == 0U)
		return 0;

	/* A run that holds something the display may read is not tried again, nor spared. */
	retained = drv_i915_lcd_show_retained(display);
	if (!retained)
		retained = drv_i915_lcd_modeset_retained(display);
	if (retained)
		return 0;

	/* Its buffers must be given back, or kept pinned for the next lighting. */
	buffers_free = 1;
	for (i = 0U; i < 2U; i++) {
		if (display->resident_buf[i].state != I915_SCANOUT_NONE &&
		    display->resident_buf[i].state != I915_SCANOUT_PINNED)
			buffers_free = 0;
	}

	/* Buffers the display may still read: not tried again, nor spared. */
	if (!buffers_free)
		return 0;

	/* The second output is the limit from here; the display is not failed for it. */
	drv_i915_head_limit(display, "the resident output could not be lit beside it");
	display->window.run_pipes = 0U;
	*spared = 1;

	/* A run that was lit ended its window for its own reasons: the next presentation lights it again. */
	if (display->resident_run_rep.display_acquired)
		return 0;

	/* Succeeded: the run never reached the display, and is tried again at once for one pipe. */
	return 1;
}
