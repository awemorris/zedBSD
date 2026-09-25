/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_vblank.c),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022-2023 Intel Corporation
 */

/*
 * The scanline, frame counter and vblank helpers of the one-screen path
 * (see vblank.h).
 *
 * The scanline and timing helpers follow the Linux 6.8.12 intel_vblank.c
 * text.  The rest is what the synchronous plane update of that text needs
 * around it: the crtc hooks that read the hardware frame counter, the
 * nesting local_irq_*() of the update section on the device's backend, the
 * vblank evasion window of the running mode, and the backend hooks of a
 * panel run that take a vblank reference, sleep until the pipe's next
 * vblank and complete the flip event.
 */

#include "modeset-internal.h"
#include "takeover-internal.h"
#include "vblank.h"
#include "pipe.h"
#include "diagnostics.h"
#include "interrupts.h"
#include "modeset.h"
#include "../mmio.h"

#include <kern/irq.h>

#include <uapi/errno.h>

static bool i915_pipe_scanline_is_moving(struct drm_i915_private *dev_priv, enum pipe pipe);
static void i915_wait_for_pipe_scanline_moving(struct intel_crtc *crtc, bool state);
static u32 i915_g4x_get_vblank_counter(struct drm_crtc *crtc);
static int i915_get_crtc_scanline(struct intel_crtc *crtc);
static int i915_crtc_scanline_offset(const struct intel_crtc_state *crtc_state);
static uint32_t i915_lcd_kernel_frame(void *ctx);

/*
 * Waits until the pipe's scanline counter stands still.
 */
void
drv_i915_wait_for_pipe_scanline_stopped(
	struct intel_crtc *crtc)
{
	/* Waits for the counter to stop. */
	i915_wait_for_pipe_scanline_moving(crtc, false);
}

/*
 * Waits until the pipe's scanline counter moves.
 */
void
drv_i915_wait_for_pipe_scanline_moving(
	struct intel_crtc *crtc)
{
	/* Waits for the counter to move. */
	i915_wait_for_pipe_scanline_moving(crtc, true);
}

/*
 * Reads the scanline the pipe is on, with interrupts off around the read.
 *
 * The Linux intel_get_crtc_scanline(): the interrupt section is the named
 * world's, on the crtc's device.
 */
int
drv_i915_get_crtc_scanline(
	struct i915_lcd_world *world,
	struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv;
	unsigned long irqflags;
	int position;

	/* Finds the device the crtc belongs to. */
	dev_priv = i915_lcd_to_i915(crtc->base.dev);

	/* Takes the scanline with interrupts off. */
	I915_LCD_LOCAL_IRQ_SAVE(world, dev_priv, irqflags);
	intel_vblank_section_enter(dev_priv);

	position = i915_get_crtc_scanline(crtc);

	intel_vblank_section_exit(dev_priv);
	I915_LCD_LOCAL_IRQ_RESTORE(world, dev_priv, irqflags);

	/* Succeeded: reports the scanline. */
	return position;
}

/*
 * Records the timings the vblank helpers read for a crtc.
 *
 * The Linux intel_crtc_update_active_timings(): the hardware mode of the
 * pipe's vblank object, the VRR vblank start, the mode flags and the
 * scanline offset.  The vblank_time_lock section guards against
 * concurrent readers that this path does not have.
 */
void
drv_i915_crtc_update_active_timings(
	const struct intel_crtc_state *crtc_state,
	bool vrr_enable)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *i915;
	u8 mode_flags;
	struct drm_display_mode adjusted_mode;
	int vmax_vblank_start;
	unsigned long irqflags;

	/* Finds the crtc and its device. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915 = i915_lcd_to_i915(crtc->base.dev);
	mode_flags = crtc_state->mode_flags;
	vmax_vblank_start = 0;

	/* Starts from the adjusted mode of the state. */
	drm_mode_init(&adjusted_mode, &crtc_state->hw.adjusted_mode);

	/*
	 * With VRR the timings run to vmax and the vblank starts at vmin;
	 * without it the VRR flag is dropped.
	 */
	if (vrr_enable) {
		I915_LCD_DRM_WARN_ON(&i915->drm, (mode_flags & I915_MODE_FLAG_VRR) == 0);

		/* The timings run to vmax; the vblank starts at vmin. */
		adjusted_mode.crtc_vtotal = crtc_state->vrr.vmax;
		adjusted_mode.crtc_vblank_end = crtc_state->vrr.vmax;
		adjusted_mode.crtc_vblank_start = intel_vrr_vmin_vblank_start(crtc_state);
		vmax_vblank_start = intel_vrr_vmax_vblank_start(crtc_state);
	} else {
		mode_flags &= ~I915_MODE_FLAG_VRR;
	}

	/* Publishes the timings the vblank and scanline helpers read. */
	I915_LCD_SPIN_LOCK_IRQSAVE(&i915->drm.vblank_time_lock, irqflags);
	intel_vblank_section_enter(i915);

	drm_calc_timestamping_constants(&crtc->base, &adjusted_mode);
	crtc->vmax_vblank_start = vmax_vblank_start;
	crtc->mode_flags = mode_flags;
	crtc->scanline_offset = i915_crtc_scanline_offset(crtc_state);

	intel_vblank_section_exit(i915);
	I915_LCD_SPIN_UNLOCK_IRQRESTORE(&i915->drm.vblank_time_lock, irqflags);
}

/*
 * Returns the crtc hooks of display version 5 and later.
 *
 * The one hook this path uses reads the hardware frame counter.  A device
 * view that is not a modeset object (the takeover registry) binds the same
 * table.
 */
const struct drm_crtc_funcs *
drv_i915_lcd_ms_crtc_funcs(void)
{
	/* i915's crtc hooks on display version 5 and later: the hardware frame counter. */
	static const struct drm_crtc_funcs crtc_funcs = {
		i915_g4x_get_vblank_counter
	};

	/* Succeeded: the table is constant and shared. */
	return &crtc_funcs;
}

/*
 * Records the active timings of a modeset object before its crtc enable.
 *
 * What the Linux intel_enable_crtc() does before the crtc_enable hook:
 * intel_crtc_update_active_timings() without VRR, with the device's vblank
 * objects bound and drm_crtc_set_max_vblank_count() of the full 32-bit
 * hardware counter.
 */
void
drv_i915_lcd_ms_active_timings(
	struct i915_lcd_modeset *ms)
{
	/* The object's device is the one the Linux text works on from here. */
	ms->world->i915_lcd_cur_i915 = &ms->i915;

	/* Binds the vblank objects and the full-width frame counter of the pipe. */
	ms->i915.drm.vblank = ms->vblank;
	ms->vblank[ms->crtc.pipe].max_vblank_count = 0xffffffffu;
	ms->crtc.base.funcs = drv_i915_lcd_ms_crtc_funcs();

	/* Records the timings of the new state. */
	drv_i915_crtc_update_active_timings(&ms->crtc_state, false);
}

/*
 * Turns interrupts off for the update section (the Linux local_irq_disable()).
 *
 * The section state is kept in the world, so a save inside the section
 * finds interrupts already off and its restore leaves them off.
 */
void
drv_i915_lcd_irq_disable(
	struct i915_lcd_world *world,
	struct drm_i915_private *i915)
{
	/* Turns interrupts off through the backend unless the section already did. */
	if (!world->i915_lcd_irqs_off)
		i915->emit->irq_off(i915->emit->ctx);

	/* From here the section has interrupts off. */
	world->i915_lcd_irqs_off = 1;
}

/*
 * Turns interrupts back on after the update section (the Linux
 * local_irq_enable()).
 */
void
drv_i915_lcd_irq_enable(
	struct i915_lcd_world *world,
	struct drm_i915_private *i915)
{
	/* Turns interrupts on through the backend when the section had them off. */
	if (world->i915_lcd_irqs_off)
		i915->emit->irq_on(i915->emit->ctx);

	/* From here the section has interrupts on. */
	world->i915_lcd_irqs_off = 0;
}

/*
 * Turns interrupts off and reports whether they were off already (the
 * Linux local_irq_save()).
 */
unsigned long
drv_i915_lcd_irq_save(
	struct i915_lcd_world *world,
	struct drm_i915_private *i915)
{
	unsigned long was_off;

	/* Remembers the state the restore goes back to. */
	was_off = (unsigned long)world->i915_lcd_irqs_off;

	/* Turns interrupts off. */
	drv_i915_lcd_irq_disable(world, i915);

	/* Succeeded: reports the state before the save. */
	return was_off;
}

/*
 * Goes back to the interrupt state a save reported (the Linux
 * local_irq_restore()).
 */
void
drv_i915_lcd_irq_restore(
	struct i915_lcd_world *world,
	struct drm_i915_private *i915,
	unsigned long was_off)
{
	/* Interrupts were on before the save: they go back on. */
	if (!was_off)
		drv_i915_lcd_irq_enable(world, i915);
}

/*
 * Computes the vblank evasion window of a modeset object's running mode.
 *
 * The window intel_pipe_update_start() sleeps out of (the Linux
 * intel_crtc_vblank_evade_scanlines()), with the old and the new crtc
 * state both the object's own.  0, or EINVAL when the window is empty.
 */
int
drv_i915_lcd_ms_evade_window(
	struct i915_lcd_modeset *ms,
	int *min,
	int *max,
	int *vblank_start)
{
	/* The object's device is the one the Linux text works on from here. */
	ms->world->i915_lcd_cur_i915 = &ms->i915;

	/* The update's atomic state is the running state on both sides. */
	ms->state.base.dev = &ms->i915.drm;
	ms->state.crtc_state = &ms->crtc_state;
	ms->state.old_crtc_state = &ms->crtc_state;

	/* Computes the window. */
	drv_i915_crtc_vblank_evade_scanlines(&ms->state, &ms->crtc, min, max, vblank_start);

	/* Refuses a window that does not start inside the frame. */
	if (*min <= 0)
		return EINVAL;
	if (*max <= 0)
		return EINVAL;

	/* Succeeded: the window lies inside the frame. */
	return 0;
}

/*
 * Returns the size of a crtc state as this unit sees it.
 *
 * A diagnostic of the environment layout: the modeset owner compares it
 * with the size the other units see before it clears one.
 */
unsigned long
drv_i915_crtc_state_size_crtc_unit(void)
{
	/* Succeeded: reports the size of this unit's crtc state. */
	return (unsigned long)sizeof(struct intel_crtc_state);
}

/*
 * Settles the pending flip event of the named world's selected screen (the
 * Linux intel_crtc_vblank_off() -> drm_crtc_vblank_off()).
 *
 * Linux sends the crtc's pending events with the current count and drops
 * the vblank reference each one held; nobody waits for them afterwards.
 * Here that is the flip's one event: the backend forgets the armed event
 * (a later wait is refused, a late vblank completes nothing), then the
 * reference goes -- exactly once, whatever happened to the flip.  The
 * event record is the waiting thread's own; the interrupt side only
 * advances the pipe's counters under the interrupt lock.
 */
void
drv_i915_lcd_ms_vblank_off(
	struct i915_lcd_world *world)
{
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;

	/* Finds the selected screen and its backend. */
	ms = &world->ms_pool[world->ms_sel];
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* Nothing to settle without an event holding a reference, or without a backend. */
	if (!ms->flip_event_ref)
		return;
	if (ms_ops == 0)
		return;

	/* The backend forgets the armed event. */
	if (ms_ops->cancel_event != 0)
		ms_ops->cancel_event(ms_ops->ctx, ms->crtc.pipe);

	/* The event's vblank reference goes back. */
	ms_ops->vblank_put(ms_ops->ctx, ms->crtc.pipe);

	/* The event no longer holds a reference; one more event was cancelled. */
	ms->flip_event_ref = 0;
	ms->events_cancelled++;
}

/*
 * Computes the vblank evasion window of the selected screen.
 *
 * I915_LCD_MS_OK with the window filled, I915_LCD_MS_NOT_PREPARED when
 * the screen is not running, or I915_LCD_MS_ERRORS when the window is
 * empty.
 */
int
drv_i915_lcd_modeset_evade_window(
	struct i915_display *display,
	int *min,
	int *max,
	int *vblank_start)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	int window_error;

	/* Finds the selected screen. */
	world = display->lcd_world;
	ms = &world->ms_pool[world->ms_sel];

	/* Only a prepared, running screen has a window. */
	if (!ms->prepared)
		return I915_LCD_MS_NOT_PREPARED;
	if (!ms->crtc.active)
		return I915_LCD_MS_NOT_PREPARED;

	/* The screen's device is the one the Linux text works on from here. */
	world->i915_lcd_cur_i915 = &ms->i915;

	/* Computes the window of the running mode. */
	window_error = drv_i915_lcd_ms_evade_window(ms, min, max, vblank_start);
	if (window_error != 0)
		return I915_LCD_MS_ERRORS;

	/* Succeeded: the window is filled in. */
	return I915_LCD_MS_OK;
}

/*
 * Takes a vblank reference on a pipe for a panel run (the backend's
 * drm_crtc_vblank_get()).
 *
 * 0, or the Linux -EINVAL the hook contract names when the pipe's
 * interrupts are not enabled.
 */
int
drv_i915_lcd_kernel_vblank_get(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;
	int error;

	/* The context is the panel run. */
	k = ctx;

	/* Takes the reference on the display interrupt state. */
	error = drv_i915_drm_vblank_get(k->d->irq, (unsigned)pipe);
	if (error != 0)
		return -I915_LCD_EINVAL;

	/* Succeeded: the pipe's vblank interrupt is on and referenced. */
	return 0;
}

/*
 * Gives a vblank reference of a panel run back (the backend's
 * drm_crtc_vblank_put()).
 */
void
drv_i915_lcd_kernel_vblank_put(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;

	/* The context is the panel run. */
	k = ctx;

	/* The last put disables the pipe's vblank interrupt at once. */
	drv_i915_drm_vblank_put(k->d->irq, (unsigned)pipe);
}

/*
 * Sleeps until the pipe's next vblank interrupt or the timeout (the
 * backend's schedule_timeout() on the pipe's vblank wait queue).
 *
 * The ticks left (at least 1) when the vblank came, 0 when it did not.
 */
long
drv_i915_lcd_kernel_vblank_sleep(
	void *ctx,
	int pipe,
	long ticks)
{
	struct i915_lcd_kernel *k;
	bool on;
	int error;

	/* The context is the panel run. */
	k = ctx;

	/*
	 * The Linux text enables local interrupts before schedule_timeout():
	 * reads the CPU's real state and puts it back, counting a sleep
	 * entered with interrupts off.
	 */
	on = kern_irq_disable();
	if (on) {
		kern_irq_enable();
	} else {
		k->sleep_irq_off++;
	}

	/* Waits for one new vblank of the pipe, at most the display ticks asked for (10 ms each, not kernel ticks). */
	error = drv_i915_wait_vblank(k->d->irq, (unsigned)pipe, 1u, (unsigned)ticks * 10u, i915_lcd_kernel_frame, k, 0);
	k->vblank_sleeps++;

	/* A broken time base is the run's anomaly, not a timeout. */
	if (error == EIO)
		drv_i915_lcd_backend_fault("time base fault while waiting for a vblank\n");

	/* Reports a sleep that ended without the vblank. */
	if (error != 0)
		return 0;

	/* A sleep that ended on the vblank leaves at least one tick. */
	if (ticks > 1)
		return ticks - 1;

	/* Succeeded: the vblank came in the last tick. */
	return 1;
}

/*
 * Arms the flip event of a panel run (the backend's
 * drm_crtc_arm_vblank_event()).
 *
 * The event completes at the pipe's next vblank after now; the vblank
 * reference was taken by the Linux text.
 */
void
drv_i915_lcd_kernel_arm_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;

	/* The context is the panel run. */
	k = ctx;

	/* Records the armed event and the frame it was armed in. */
	k->event_armed = 1;
	k->event_pipe = pipe;
	k->event_frame = i915_lcd_kernel_frame(k);
}

/*
 * Waits for the armed flip event of a panel run.
 *
 * The event completes with a new vblank interrupt of the pipe after the
 * wait began and the frame counter past the arming frame.  0; the Linux
 * -EINVAL when no event of the pipe is armed, -ETIMEDOUT when it did not
 * complete within timeout_ms, -EIO when the time base failed.
 */
int
drv_i915_lcd_kernel_wait_event(
	void *ctx,
	int pipe,
	unsigned timeout_ms)
{
	struct i915_lcd_kernel *k;
	uint32_t frame;
	int error;

	/* The context is the panel run. */
	k = ctx;

	/* Refuses a wait for an event that is not armed on this pipe. */
	if (!k->event_armed)
		return -I915_LCD_EINVAL;
	if (pipe != k->event_pipe)
		return -I915_LCD_EINVAL;

	/* Waits for a new vblank interrupt of the pipe. */
	error = drv_i915_wait_vblank(k->d->irq, (unsigned)pipe, 1u, timeout_ms, i915_lcd_kernel_frame, k, 0);

	/* A vblank that left the frame counter at the arming frame did not complete the event. */
	if (error == 0) {
		frame = i915_lcd_kernel_frame(k);
		if (frame == k->event_frame)
			error = ETIMEDOUT;
	}

	/* Reports a broken time base. */
	if (error == EIO)
		return I915_LCD_EIO;

	/* Reports an event that did not complete in time. */
	if (error != 0)
		return I915_LCD_ETIMEDOUT;

	/* The event is complete: it is no longer armed. */
	k->event_armed = 0;

	/* Succeeded: the flip event completed. */
	return 0;
}

/*
 * Forgets the armed flip event of a panel run (the backend's
 * drm_crtc_vblank_off() on a pending event).
 *
 * A later wait is refused and a late vblank completes nothing.  The event
 * record is the run's own; the interrupt handler only advances the pipe's
 * counters under the interrupt lock.
 */
void
drv_i915_lcd_kernel_cancel_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;

	UNUSED_PARAMETER(pipe);

	/* The context is the panel run. */
	k = ctx;

	/* Counts an event that was still armed. */
	if (k->event_armed)
		k->events_cancelled++;

	/* The event is forgotten. */
	k->event_armed = 0;
}

/* Tells whether the pipe's scanline counter moves across 5 ms (the Linux pipe_scanline_is_moving()). */
static bool
i915_pipe_scanline_is_moving(
	struct drm_i915_private *dev_priv,
	enum pipe pipe)
{
	i915_reg_t reg;
	u32 line1;
	u32 line2;

	/* The scanline register of the pipe. */
	reg = PIPEDSL(pipe);

	/* Reads the scanline, waits 5 ms, reads it again. */
	line1 = i915_lcd_intel_de_read(dev_priv, reg) & PIPEDSL_LINE_MASK;
	i915_lcd_msleep(dev_priv, 5);
	line2 = i915_lcd_intel_de_read(dev_priv, reg) & PIPEDSL_LINE_MASK;

	/* The counter stood still. */
	if (line1 == line2)
		return false;

	/* Succeeded: the counter moved. */
	return true;
}

/* Waits up to 100 ms for the scanline counter to reach a state (the Linux wait_for_pipe_scanline_moving()). */
static void
i915_wait_for_pipe_scanline_moving(
	struct intel_crtc *crtc,
	bool state)
{
	struct drm_i915_private *dev_priv;
	enum pipe pipe;
	int wait_error;

	/* Finds the device and the pipe. */
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	pipe = crtc->pipe;

	/* Waits for the display line to settle or start moving; the sleeps are the device's. */
	wait_error = I915_LCD_WAIT_FOR(dev_priv, i915_pipe_scanline_is_moving(dev_priv, pipe) == state, 100);
	if (wait_error != 0) {
		I915_LCD_DRM_ERR(&dev_priv->drm,
				 "pipe %c scanline %s wait timed out\n",
				 pipe_name(pipe),
				 i915_lcd_str_on_off(state));
	}
}

/*
 * Reads the pipe's hardware frame counter (the Linux g4x_get_vblank_counter(),
 * the crtc hook of display version 5 and later).
 *
 * 0 while the pipe's vblank object has no counter range.
 */
static u32
i915_g4x_get_vblank_counter(
	struct drm_crtc *crtc)
{
	struct drm_i915_private *dev_priv;
	struct drm_vblank_crtc *vblank;
	enum pipe pipe;
	u32 frame;

	/* Finds the device, the pipe's vblank object and the pipe. */
	dev_priv = i915_lcd_to_i915(crtc->dev);
	vblank = &dev_priv->drm.vblank[drm_crtc_index(crtc)];
	pipe = to_intel_crtc(crtc)->pipe;

	/* A vblank object without a counter range has no counter. */
	if (!vblank->max_vblank_count)
		return 0;

	/* Reads the counter. */
	frame = i915_lcd_intel_de_read(dev_priv, PIPE_FRMCOUNT_G4X(pipe));

	/* Succeeded: reports the frame number. */
	return frame;
}

/*
 * Reads the pipe's scanline, adjusted by the crtc's scanline offset (the
 * Linux __intel_get_crtc_scanline()).
 *
 * A fast read of the display block: no forcewake.  0 while the crtc is off.
 */
static int
i915_get_crtc_scanline(
	struct intel_crtc *crtc)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	const struct drm_display_mode *mode;
	struct drm_vblank_crtc *vblank;
	enum pipe pipe;
	int position;
	int vtotal;
	int i;
	int temp;

	/* Finds the device and the pipe. */
	dev = crtc->base.dev;
	dev_priv = i915_lcd_to_i915(dev);
	pipe = crtc->pipe;

	/* A crtc that is off is on no scanline. */
	if (!crtc->active)
		return 0;

	/* Takes the hardware mode the active timings recorded. */
	vblank = &crtc->base.dev->vblank[drm_crtc_index(&crtc->base)];
	mode = &vblank->hwmode;

	/* DSI takes the scanline from the vblank timestamp instead (not reached here). */
	if (crtc->mode_flags & I915_MODE_FLAG_GET_SCANLINE_FROM_TIMESTAMP) {
		position = __intel_get_crtc_scanline_from_timestamp(crtc);
		return position;
	}

	/* An interlaced mode counts lines per field. */
	vtotal = mode->crtc_vtotal;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		vtotal /= 2;

	/* Reads the scanline counter. */
	position = i915_lcd_intel_de_read_fw(dev_priv, PIPEDSL(pipe)) & PIPEDSL_LINE_MASK;

	/*
	 * On HSW the DSL register appears to return 0 when read just before
	 * the start of vblank: it is read again, up to 100 times 1 us apart,
	 * so the update does not appear to span a vblank.
	 */
	if (HAS_DDI(dev_priv) && !position) {
		for (i = 0; i < 100; i++) {
			i915_lcd_udelay(dev_priv, 1);
			temp = i915_lcd_intel_de_read_fw(dev_priv, PIPEDSL(pipe)) & PIPEDSL_LINE_MASK;
			if (temp != position) {
				position = temp;
				break;
			}
		}
	}

	/* Succeeded: reports the scanline with the offset of the output type. */
	return (position + crtc->scanline_offset) % vtotal;
}

/*
 * Computes the offset between the scanline counter and the real scanline
 * (the Linux intel_crtc_scanline_offset()).
 *
 * The counter increments at the leading edge of hsync and starts from
 * vtotal - 1 on the first active line, so it reads one less than expected:
 * the offset is 1.  On gen2 it counts from 1 (vtotal - 1 is added instead),
 * and on HSW+ an HDMI output adds one more line (2).
 */
static int
i915_crtc_scanline_offset(
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	const struct drm_display_mode *adjusted_mode;
	int vtotal;
	bool hdmi;

	/* Finds the device and the adjusted mode. */
	i915 = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	adjusted_mode = &crtc_state->hw.adjusted_mode;

	/* Gen2 counts from 1: the offset is a field's vtotal - 1. */
	if (I915_LCD_DISPLAY_VER(i915) == 2) {
		vtotal = adjusted_mode->crtc_vtotal;
		if (adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE)
			vtotal /= 2;

		return vtotal - 1;
	}

	/* An HDMI output on a DDI platform is one line further. */
	if (HAS_DDI(i915)) {
		hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
		if (hdmi)
			return 2;
	}

	/* Succeeded: the counter reads one line behind. */
	return 1;
}

/*
 * Reads the frame counter a panel run compares against (the frame hook of
 * the run's vblank waits).
 *
 * XXX: the register is looked up by name in the observation table, which
 * the lookup fills for pipe A: this is PIPE_FRMCOUNT_G4X of pipe A for
 * every pipe, as the old backend read it.
 */
static uint32_t
i915_lcd_kernel_frame(
	void *ctx)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	uint32_t reg;
	uint32_t frame;

	/*
	 * The context is the panel run, which is the display's own (struct
	 * i915_display.lk): the display and its modeset world, whose
	 * observation table the lookup fills, are the run's owner.
	 */
	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* Finds the frame counter register and reads it. */
	reg = drv_i915_lcd_reg_by_name(display->lcd_world, "PIPE_FRMCOUNT_G4X");
	frame = drv_i915_read32(k->d->mmio, reg);

	/* Succeeded: reports the frame number. */
	return frame;
}
