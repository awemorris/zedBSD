/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A13: a change of the monitor on the firmware's output.
 *
 * The interrupt thread moves the display's topology when the console's
 * hot-plug line changes and asks the bring-up thread to act, which it
 * does between stages, after the line has settled: a monitor that went
 * away has the picture blanked and the transmitter turned off (the timing
 * generator runs on, so frames and their events go on); a monitor that
 * came has its EDID read again and the output lit again in the mode
 * shown, when it is the same monitor or one that shows that mode, else in
 * the mode the new monitor prefers, which moves the topology once more so
 * the compositor queries the display again.  Root's control request and
 * the stage itself act as if the line had read a level, which checks the
 * output's off and on without anybody at the cable.
 *
 * It takes the node's present and display locks (when the GPU device has
 * a display) and the display's mode lock, in that order, so it never
 * runs inside a presentation or a mode set.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"
#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-sink.h"

/* The levels a request acts on: the line's own, a monitor gone, a monitor there. */
#define HOTPLUG_LINE			0U
#define HOTPLUG_GONE			1U
#define HOTPLUG_THERE			2U

/*
 * How long a changed line settles before it is read (100 ms), how long
 * stage A13 keeps the output dark (1 s), a step of the waits, the frames
 * the stage looks at and the longest wait for one frame.
 */
#define HOTPLUG_SETTLE_US		100000U
#define HOTPLUG_DARK_US			1000000U
#define HOTPLUG_STEP_US			1000U
#define HOTPLUG_LOOK_FRAMES		30U
#define HOTPLUG_FRAME_WAIT_US		50000U

static int hotplug_run(struct amdrdna_device *device, unsigned asked_level);
static int hotplug_gone(struct amdrdna_device *device);
static int hotplug_there(struct amdrdna_device *device);
static uint32_t hotplug_frames(struct amdrdna_device *device, unsigned frames);

/*
 * Stage A13: turns the output off as if the monitor went away, keeps it
 * dark a second, then reads the monitor again and lights the output again
 * in the mode shown; the frames must go on without underflow.
 *
 * Returns 0; ENOENT or ENOTSUP without a TMDS output path (stage A10
 * first); EIO when the frames stop or underflow; or the error of a step.
 */
int
amdrdna_a13_hotplug(
	struct amdrdna_device *device)
{
	uint32_t frames;
	uint32_t underflow;
	uint32_t timing_underflow;
	uint64_t dark_from;
	uint64_t dark_us;
	int error;

	/* The output off, as if the monitor went away. */
	amdrdna_mark("A13 begin");
	dark_from = amdrdna_display_now_us();
	error = hotplug_run(device, HOTPLUG_GONE);
	if (error != 0) {
		amdrdna_mark("A13 fail: the output did not go off (%d)", error);
		return error;
	}

	/* Dark for a while; the frames go on. */
	kern_usleep_range(HOTPLUG_DARK_US, HOTPLUG_DARK_US + HOTPLUG_STEP_US);
	frames = hotplug_frames(device, 2U);
	if (frames < 2U) {
		amdrdna_mark("A13 fail: the frames stopped while the output was dark");
		(void)hotplug_run(device, HOTPLUG_THERE);
		return EIO;
	}

	/* The monitor read again and the output lit again. */
	error = hotplug_run(device, HOTPLUG_THERE);
	dark_us = amdrdna_display_now_us() - dark_from;
	if (error != 0) {
		amdrdna_mark("A13 fail: the output did not come back (%d)", error);
		return error;
	}

	/* The frames and the underflow after. */
	(void)amdrdna_display_underflow(device, &timing_underflow);
	frames = hotplug_frames(device, HOTPLUG_LOOK_FRAMES);
	underflow = amdrdna_display_underflow(device, &timing_underflow);
	if (frames < HOTPLUG_LOOK_FRAMES ||
	    underflow != 0U ||
	    timing_underflow != 0U) {
		amdrdna_mark("A13 fail: frames=%u underflow=%u/%u after the output came back", (unsigned)frames, (unsigned)underflow,
		    (unsigned)timing_underflow);
		return EIO;
	}

	/* Says how it went. */
	amdrdna_mark("A13 ok output off and on %ux%u@%u.%03u dark=%llums frames=%u underflow=0/0 edid=%s modes=%u",
	    device->display.width,
	    device->display.height,
	    (unsigned)(device->display.refresh_millihz / 1000U),
	    (unsigned)(device->display.refresh_millihz % 1000U),
	    (unsigned long long)(dark_us / 1000U),
	    (unsigned)frames,
	    device->sink.edid.vendor,
	    device->sink.usable_modes);

	/* Succeeded: the output went off and came back. */
	return 0;
}

/*
 * Asks the bring-up thread to act on the console's hot-plug line (the
 * interrupt thread, after the line changed).  Nothing is asked before
 * stage A10 read the output's path, or once the machine stops.
 */
void
amdrdna_hotplug_request(
	struct amdrdna_device *device)
{
	const struct amdrdna_sink *sink;
	unsigned long irq;

	/* Only an output stage A10 read. */
	sink = amdrdna_sink_ready(device);
	if (sink == NULL)
		return;

	/*
	 * requested makes the thread act once on the line's own level (a
	 * waiting control request's level is kept).
	 */
	irq = spin_lock_irqsave(&device->state_lock);

	if (!device->stopping) {
		device->hotplug_work.requested = true;
		waitq_wake_all(&device->state_waitq);
	}

	spin_unlock_irqrestore(&device->state_lock, irq);
}

/*
 * Asks the bring-up thread to act as if the line read a level (root's
 * control request: HOTPLUG_GONE or HOTPLUG_THERE) and waits until it did;
 * one caller at a time.
 *
 * Returns what the thread's action returned (0, or its error); EINVAL for
 * another level; ENOENT before stage A10; ENODEV when the thread no longer
 * runs.
 */
int
amdrdna_hotplug_ask(
	struct amdrdna_device *device,
	unsigned asked_level)
{
	const struct amdrdna_sink *sink;
	unsigned long irq;
	uint64_t sequence;
	uint64_t start;
	int error;

	/* A level the request names, on an output stage A10 read. */
	if (asked_level != HOTPLUG_GONE && asked_level != HOTPLUG_THERE)
		return EINVAL;
	sink = amdrdna_sink_ready(device);
	if (sink == NULL)
		return ENOENT;

	/* One caller at a time waits for the thread. */
	mutex_lock(&device->hotplug_work.lock);

	/* Asks the thread, then waits for it to act or to stop. */
	irq = spin_lock_irqsave(&device->state_lock);

	error = 0;
	if (device->stopping || !device->thread_running) {
		error = ENODEV;
	} else {
		start = device->hotplug_work.finished;
		device->hotplug_work.requested = true;
		device->hotplug_work.asked_level = asked_level;
		waitq_wake_all(&device->state_waitq);
		while (device->hotplug_work.finished == start && !device->stopping) {
			sequence = waitq_sequence(&device->state_waitq);
			(void)waitq_sleep(&device->state_waitq, &device->state_lock, sequence, 0U, 0U);
		}
		error = ENODEV;
		if (device->hotplug_work.finished != start)
			error = device->hotplug_work.result;
	}

	spin_unlock_irqrestore(&device->state_lock, irq);

	mutex_unlock(&device->hotplug_work.lock);

	/* Reports an action that was refused or failed. */
	if (error != 0)
		return error;

	/* Succeeded: the thread acted. */
	return 0;
}

/*
 * Serves a change of the monitor on the bring-up thread, which holds the
 * device's state lock when it calls this and gets it back held; the lock
 * is let go while the action runs.  Returns the irq word of the held lock.
 */
unsigned long
amdrdna_hotplug_serve(
	struct amdrdna_device *device,
	unsigned long irq)
{
	unsigned asked_level;
	int error;

	/* Takes the request and its level. */
	asked_level = device->hotplug_work.asked_level;
	device->hotplug_work.requested = false;
	device->hotplug_work.asked_level = HOTPLUG_LINE;

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* Acts without the lock: it sleeps and runs the video BIOS. */
	error = hotplug_run(device, asked_level);

	/* Publishes the result and wakes a waiting caller. */
	irq = spin_lock_irqsave(&device->state_lock);

	device->hotplug_work.result = error;
	device->hotplug_work.finished++;
	waitq_wake_all(&device->state_waitq);

	/* Succeeded: the lock is held again. */
	return irq;
}

/*
 * Acts on a level: the line's own after it settled, or the one asked for;
 * under the node's present and display locks (when the GPU device has a
 * display) and the display's mode lock.  Returns 0 or the action's error.
 */
static int
hotplug_run(
	struct amdrdna_device *device,
	unsigned asked_level)
{
	struct amdrdna_node *node;
	bool monitor_in;
	int error;

	/* The line settles, then its level; a request names its own. */
	if (asked_level == HOTPLUG_LINE) {
		kern_usleep_range(HOTPLUG_SETTLE_US, HOTPLUG_SETTLE_US + HOTPLUG_STEP_US);
		monitor_in = amdrdna_display_sense(device);
	} else {
		monitor_in = false;
		if (asked_level == HOTPLUG_THERE)
			monitor_in = true;
	}

	/* The node's locks when the GPU device has a display: no presentation runs meanwhile. */
	node = device->node;
	if (node != NULL && node->display == 0)
		node = NULL;
	if (node != NULL) {
		mutex_lock(&node->present_lock);
		mutex_lock(&node->display_lock);
	}

	/* A stopped display is left alone. */
	error = 0;
	if (node == NULL || node->stopped == 0) {
		mutex_lock(&device->display.mode_lock);

		if (monitor_in) {
			error = hotplug_there(device);
		} else {
			error = hotplug_gone(device);
		}

		mutex_unlock(&device->display.mode_lock);
	}

	if (node != NULL) {
		mutex_unlock(&node->display_lock);
		mutex_unlock(&node->present_lock);
	}

	/* Reports an action that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the output follows the monitor. */
	return 0;
}

/*
 * Turns the output off for a monitor that went away (once: an output
 * already dark is left).  The caller holds the display's mode lock.
 */
static int
hotplug_gone(
	struct amdrdna_device *device)
{
	int error;

	/* Already dark. */
	if (device->display.output_dark) {
		amdrdna_mark("A13 unplug: the output is already off");
		return 0;
	}

	/* The picture blanked and the transmitter off. */
	error = amdrdna_mode_output(device, false);
	if (error != 0) {
		amdrdna_mark("A13 unplug: the output's off failed (%d)", error);
		return error;
	}

	/* Says so. */
	amdrdna_mark("A13 unplug: the output is off, the timing generator runs on");

	/* Succeeded: the output is dark. */
	return 0;
}

/*
 * Reads the monitor that came and lights the output again: in the mode
 * shown when it is the same monitor (the same EDID), one that shows that
 * mode, or one whose EDID did not read; else in the mode the new monitor
 * prefers, with the topology moved on.  The caller holds the node's
 * display lock (when there is one) and the display's mode lock.
 */
static int
hotplug_there(
	struct amdrdna_device *device)
{
	struct amdrdna_sink *next;
	struct amdrdna_timing now;
	struct amdrdna_timing preferred;
	bool same_mode;
	bool found;
	int compared;
	int read;
	int error;

	/* The mode shown. */
	error = amdrdna_mode_current(device, &now);
	if (error != 0)
		return error;

	/* The monitor's EDID again, into the thread's own record. */
	next = &device->sink_next;
	read = amdrdna_sink_read(device, next);

	/* Keeps the mode shown for the same monitor, one that shows it, or one that did not answer. */
	same_mode = true;
	if (read == 0) {
		compared = -1;
		if (next->edid_length == device->sink.edid_length)
			compared = kern_memcmp(next->edid_bytes, device->sink.edid_bytes, next->edid_length);
		if (compared != 0)
			same_mode = amdrdna_sink_carries(next, &now);

		/* The new record replaces the published one (the readers hold the node's display lock). */
		next->ready = true;
		device->sink = *next;
	}

	/* The same mode: nothing to do on an output that is lit, else the stream and the transmitter on. */
	if (same_mode) {
		if (!device->display.output_dark) {
			amdrdna_mark("A13 plug: the output is lit in %ux%u (edid=%d)", now.width, now.height, read);
			return 0;
		}
		error = amdrdna_mode_output(device, true);
		if (error != 0) {
			amdrdna_mark("A13 plug: the output did not come on (%d)", error);
			return error;
		}
		amdrdna_mark("A13 plug: the output is lit again in %ux%u@%u.%03u (edid=%d %s)",
		    now.width,
		    now.height,
		    (unsigned)(now.refresh_millihz / 1000U),
		    (unsigned)(now.refresh_millihz % 1000U),
		    read,
		    device->sink.edid.vendor);
		return 0;
	}

	/* Another monitor that does not show the mode: its preferred one, if it shows any. */
	found = amdrdna_sink_preferred(&device->sink, &preferred);
	if (!found) {
		amdrdna_mark("A13 plug: the new monitor shows none of its modes here; the output stays off");
		return ERANGE;
	}
	error = amdrdna_mode_set(device, &preferred);

	/* The compositor learns of the new mode by the topology, whatever the set did. */
	amdrdna_display_topology_move(device);
	if (error != 0) {
		amdrdna_mark("A13 plug: the new monitor's mode %ux%u was not set (%d)", preferred.width, preferred.height, error);
		return error;
	}
	amdrdna_mark("A13 plug: another monitor, now %ux%u@%u.%03u",
	    preferred.width,
	    preferred.height,
	    (unsigned)(preferred.refresh_millihz / 1000U),
	    (unsigned)(preferred.refresh_millihz % 1000U));

	/* Succeeded: the output shows the new monitor's mode. */
	return 0;
}

/* Waits for frames of the console's timing generator (at most 50 ms each); reports how many passed. */
static uint32_t
hotplug_frames(
	struct amdrdna_device *device,
	unsigned frames)
{
	uint32_t first;
	uint32_t now;
	uint32_t passed;
	unsigned waited;

	/* The frame counter until it moved on that far (a 24-bit counter). */
	first = amdrdna_display_frame_count(device);
	passed = 0U;
	for (waited = 0U; waited < HOTPLUG_FRAME_WAIT_US * frames; waited += HOTPLUG_STEP_US) {
		now = amdrdna_display_frame_count(device);
		passed = (now - first) & 0x00ffffffU;
		if (passed >= frames)
			break;
		kern_usleep_range(HOTPLUG_STEP_US, HOTPLUG_STEP_US * 2U);
	}

	/* Succeeded: the frames that passed. */
	return passed;
}
