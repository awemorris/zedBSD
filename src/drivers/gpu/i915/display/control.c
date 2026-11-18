/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display control of the resident output.
 *
 * Refresh: the boundaries are the pipe's own frames.  The resident run
 * leaves the vblank interrupt masked, so the pipe's hardware frame counter
 * (PIPE_FRMCOUNT) is read each millisecond while a caller waits and
 * extended to a count that never goes back: a counter that went back is a
 * pipe lit again, and only what it counts from then on is added.  The
 * counter is read only while the display window is up, under the lock the
 * worker takes to lower display_up before the pipe stops.
 *
 * Power: the eDP panel's light is switched off or on by the worker (the
 * pipe keeps running, so SUSPEND is the same as OFF), for the open holding
 * the lease once a frame of it is shown; the lease's end switches it on.
 */

#include "internal.h"
#include "backlight.h"
#include "control.h"
#include "output.h"
#include "present.h"
#include "xd-pipe.h"

#include "../i915.h"
#include "../worker.h"

#include <kern/clock.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>

/* How long a refresh wait sleeps before it looks again, in milliseconds (a tick at least). */
#define I915_REFRESH_POLL_MS 1U

static void i915_refresh_sample(struct i915_device *device, uint64_t *count, uint64_t *time_ns, int *lit);

/*
 * Prepares the refresh boundaries of a display: no count yet.
 */
void
drv_i915_display_refresh_init(
	struct i915_display *display)
{
	/* The lock, and nothing counted. */
	spin_init(&display->refresh.lock, LOCK_RANK_DEVICE, "i915 refresh");
	display->refresh.count = 0U;
	display->refresh.time_ns = 0U;
	display->refresh.frame = 0U;
	display->refresh.frame_valid = 0;
}

/*
 * Raises or lowers the display window's display_up under the refresh lock.
 *
 * Runs on the worker: raised once the pipe runs (its frame counter is then
 * read afresh), lowered before the pipe stops.
 */
void
drv_i915_display_refresh_up(
	struct i915_display *display,
	int up)
{
	unsigned long irq;

	/* The window's state; a pipe lit again starts a new frame counter. */
	irq = spin_lock_irqsave(&display->refresh.lock);

	display->window.display_up = up;
	display->refresh.frame_valid = 0;

	spin_unlock_irqrestore(&display->refresh.lock, irq);
}

/*
 * Waits for the resident output's refresh boundary after the request's
 * cursor (GPU_DISPLAY_REFRESH), or reports the count at once for cursor 0.
 *
 * lit_possible is 0 for an output the node does not light: it has no
 * boundary.  Returns 0 with sequence and time_ns, or ETIMEDOUT.
 */
int
drv_i915_display_refresh_wait(
	struct i915_device *device,
	int lit_possible,
	struct gpu_display_refresh *request)
{
	uint64_t deadline;
	uint64_t now;
	uint64_t count;
	uint64_t time_ns;
	int lit;

	/* The deadline in scheduler ticks, rounded up. */
	now = sched_ticks();
	deadline = now + kern_ns_to_ticks(request->timeout_ns);

	/* Until a boundary after the cursor, or the deadline. */
	for (;;) {
		/* The count now, while the pipe runs. */
		count = 0U;
		time_ns = 0U;
		lit = 0;
		if (lit_possible)
			i915_refresh_sample(device, &count, &time_ns, &lit);

		/* Cursor zero, or a boundary after the cursor of a running pipe: reported. */
		if (request->cursor == 0U || (lit && count > request->cursor)) {
			request->sequence = count;
			request->time_ns = time_ns;
			request->flags = 0U;
			break;
		}

		/* No boundary came in time. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Looks again a millisecond later. */
		sched_sleep(now + kern_ms_to_ticks(I915_REFRESH_POLL_MS));
	}

	/* Succeeded: the boundary after the cursor. */
	return 0;
}

/*
 * Switches the panel's light off (off 1: OFF or SUSPEND) or on for the open
 * holding the lease (GPU_DISPLAY_POWER).
 *
 * Returns 0, EBUSY for another open's lease or none, or while no frame of
 * the lease is shown yet (the panel is not lit by the driver), or the
 * worker's error.
 */
int
drv_i915_display_power_set(
	struct i915_device *device,
	void *session,
	int off)
{
	struct i915_resident_display *rd;
	uint32_t on;
	int error;

	/* The lease's state under its mutex. */
	rd = &device->display->rd;
	drv_i915_present_lease_init(device->display);
	mutex_lock(&rd->mutex);

	/* Only this open's lease powers the panel, once its frames are shown. */
	if (rd->owner != session || rd->lease == 0U || !rd->active) {
		mutex_unlock(&rd->mutex);
		return EBUSY;
	}

	/* A state the panel has already is done. */
	if (off == rd->power_off) {
		mutex_unlock(&rd->mutex);
		return 0;
	}

	/* The worker switches the light. */
	on = 1U;
	if (off)
		on = 0U;
	error = drv_i915_worker_sync_backlight(device, I915_BACKLIGHT_POWER, &on);
	if (error == 0)
		rd->power_off = off;

	mutex_unlock(&rd->mutex);

	/* Reports why the light was not switched. */
	if (error != 0)
		return error;

	/* Succeeded: the panel has the requested power. */
	return 0;
}

/*
 * Switches the panel's light on again at the end of a lease that had it off
 * (the lease mutex held).
 */
void
drv_i915_display_power_restore_locked(
	struct i915_device *device)
{
	struct i915_resident_display *rd;
	uint32_t on;

	/* Nothing to restore. */
	rd = &device->display->rd;
	if (!rd->power_off)
		return;

	/* The light comes back while the panel still shows the lease's frames; the flag goes either way. */
	on = 1U;
	if (rd->active)
		(void)drv_i915_worker_sync_backlight(device, I915_BACKLIGHT_POWER, &on);
	rd->power_off = 0;
}

/* Reads the pipe's frame counter into the count, while the window is up. */
static void
i915_refresh_sample(
	struct i915_device *device,
	uint64_t *count,
	uint64_t *time_ns,
	int *lit)
{
	struct i915_display *display;
	unsigned long irq;
	uint32_t frame;
	int pipe;

	/* The resident output's pipe: the panel's A, or another output's (output.c says which). */
	display = device->display;
	pipe = (int)drv_i915_display_output_pipe(&display->output);

	/* Counts what the counter moved since the last read, while the pipe runs. */
	irq = spin_lock_irqsave(&display->refresh.lock);

	*lit = 0;
	if (display->window.display_up && !display->output.none) {
		*lit = 1;
		frame = drv_i915_xd_pipe_frame(&display->xd.io, (unsigned)pipe);
		if (display->refresh.frame_valid && frame > display->refresh.frame) {
			display->refresh.count += frame - display->refresh.frame;
			display->refresh.time_ns = kern_ticks_to_ns(sched_ticks());
		}

		/* The next read counts from this frame. */
		display->refresh.frame = frame;
		display->refresh.frame_valid = 1;
	}

	/* The count and its time, moved or not. */
	*count = display->refresh.count;
	*time_ns = display->refresh.time_ns;

	spin_unlock_irqrestore(&display->refresh.lock, irq);
}
