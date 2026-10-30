/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A8: the display of the amdrdna GPU device.
 *
 * One display: the output the firmware lit and the driver took over
 * (display.c), in the firmware's mode or another the monitor lists that a
 * present of its size set (modeset.c; the lease's end sets the
 * firmware's again for the console).  One lease at a
 * time; FIFO presentation of storage frames (copied by the CPU into one
 * of the display's two frames, then flipped to) and of blobs (flipped to
 * as they are, when their rows are the display's).  A presentation
 * returns once its flip has begun, so a wait answers at once.  Releasing
 * the lease, closing its session or destroying the blob on the screen
 * flips back to the console's surface, the firmware's.
 *
 * The kernel's text console draws into that surface through an uncached
 * mapping, a whole repaint for each line that scrolls.  While a frame of
 * the lease is on the screen nobody sees it, so it is kept from drawing
 * (it keeps its text); a kernel message logged then costs no time of the
 * presentation's or the executor's.  Once it is stopped the surface is
 * painted black (the boot logo goes), and it stays stopped past the
 * lease's end: a game that takes the display over from the compositor,
 * or gives it back, ends one lease and claims the next a few frames
 * later, and the flip back in between shows that black, not the console.
 * The console draws again only after PRESENT_CONSOLE_REST_MS with no
 * lease, or at the stop for the reboot.
 *
 * The node's present lock puts the presentations, the lease's end and a
 * blob's going off the screen in one order; a presentation takes the
 * display lock only to check the lease and to write the flip, and waits
 * for the frame's update without it, so the cursor is not held up by the
 * refresh.  A flip that does not begin breaks the display: nothing is
 * flipped after it, presentations fail with EIO, and a blob that may
 * still be on the screen keeps its VRAM until power-off.
 *
 * The display's events are its topology sequence, which a plug or an
 * unplug of the monitor moves.  Its control: refresh boundaries (the
 * timing generator's frame counter) and power, which blanks the plane
 * (the output stays lit, showing black) while the lease's holder asks.
 * The cursor plane is the plane's hardware cursor, 64 by 64.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <drivers/gpu/gpu-scanout.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"

/* The one display's identifier and its generation, which never changes. */
#define PRESENT_DISPLAY_ID		1U
#define PRESENT_GENERATION		1U

/* The cursor's side, the largest a lease may set, and the bytes of one pixel. */
#define PRESENT_CURSOR_SIDE		64U
#define PRESENT_PIXEL_BYTES		4U

/*
 * The plane reads a surface from a 256-byte boundary with rows a multiple
 * of 256 bytes apart.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/amdgpu_display.c
 * (amdgpu_display_verify_sizes, amdgpu_display_verify_plane: a linear
 * surface's 256-byte block).
 */
#define PRESENT_OFFSET_ALIGN		256U
#define PRESENT_STRIDE_ALIGN		256U

/* The longest refresh wait: a second, as the interface bounds it. */
#define PRESENT_REFRESH_MAX_NS		1000000000ULL

/*
 * How long the display goes without a lease before the text console draws
 * again, and how often the rest thread looks.
 */
#define PRESENT_CONSOLE_REST_MS		3000U

/*
 * A presentation that takes PRESENT_SLOW_MS or more is marked with where
 * its time went (the first PRESENT_SLOW_MARKS of them): the wait for the
 * presentation lock, the admission and copy, the flip's write and the
 * wait for the flip; PRESENT_TIMES moments are taken.
 */
#define PRESENT_SLOW_MS			34U
#define PRESENT_SLOW_MARKS		40U
#define PRESENT_TIMES			5U
#define PRESENT_REST_LOOK_MS		250U

static int present_query(void *private_data, void *session, struct gpu_display_info *request);
static int present_mode(void *private_data, void *session, struct gpu_display_mode *request);
static int present_claim(void *private_data, void *session, struct gpu_display_claim *request);
static int present_release(void *private_data, void *session, const struct gpu_display_release *request);
static void present_slow_mark(struct amdrdna_node *node, const uint64_t *times);
static int present_present(void *private_data, void *session, void *object, struct gpu_display_present *request);
static int present_wait(void *private_data, void *session, struct gpu_display_wait *request);
static int present_events(void *private_data, void *session, uint64_t *sequence);
static int present_power(void *private_data, void *session, const struct gpu_display_power *request);
static int present_refresh(void *private_data, void *session, struct gpu_display_refresh *request);
static int present_cursor_set(void *private_data, void *session, void *object, const struct gpu_display_cursor_image *request);
static int present_cursor_move(void *private_data, void *session, const struct gpu_display_cursor_move *request);
static int present_cursor_hide(void *private_data, void *session, const struct gpu_display_cursor_hide *request);
static int present_query_device(void *private_data, void *session, struct gpu_device_info *request);
static int present_constraints(void *private_data, void *session, struct gpu_scanout_constraints *request);

/*
 * The display operations of the amdrdna GPU device: query, mode, claim,
 * release, present and wait, the topology sequence, power and refresh
 * (the display control), and the cursor plane.  Presentations wait under
 * the open's admission (no ungated flag, no adoption wait); no overlay
 * planes.  Shared by every node; immutable.
 */
static const struct drv_gpu_display_ops present_display_ops = {
	present_query,
	present_mode,
	present_claim,
	present_release,
	present_present,
	present_wait,
	present_events,
	present_power,
	present_refresh,
	present_cursor_set,
	present_cursor_move,
	present_cursor_hide,
	NULL,
	0U,
	NULL,
	NULL
};

/*
 * The scanout operations of the amdrdna GPU device: the pairing query and
 * the display's constraints; no foreign import.  Shared by every node;
 * immutable.
 */
static const struct drv_gpu_scanout_ops present_scanout_ops = {
	present_query_device,
	present_constraints,
	NULL
};

static int present_admit(struct amdrdna_node *node, void *session, const struct amdrdna_resource *resource, const struct gpu_display_present *request, bool blob, uint64_t *address);
static void present_copy(struct amdrdna_node *node, const struct amdrdna_resource *resource, const struct gpu_display_present *request);
static int present_resize(struct amdrdna_node *node, const struct gpu_display_present *request);
static void present_firmware_mode(struct amdrdna_node *node);
static void present_end_lease(struct amdrdna_node *node);
static void present_broken(struct amdrdna_node *node, int error);
static bool present_text_hold(struct amdrdna_node *node);
static void present_text_give_back(struct amdrdna_node *node);
static void present_console_paint(struct amdrdna_node *node);
static void present_rest_watch(void *argument);
static int present_cursor_check(struct amdrdna_node *node, void *session, uint64_t lease, uint64_t generation);
static void present_cursor_copy(struct amdrdna_node *node, const struct amdrdna_resource *resource, const struct gpu_display_cursor_image *request, unsigned slot);

/*
 * Runs stage A8: gives the GPU device the display's operations and
 * registers it with the GPU core, which publishes /dev/gpuN.
 *
 * Returns 0 when the device is registered with its display (or the
 * display stays the firmware's), ENODEV without stage A5s's device or
 * stages A6 and A7, EBUSY for a device registered before, or the core's
 * error.
 */
int
amdrdna_a8_register(
	struct amdrdna_device *device)
{
	struct amdrdna_node *node;
	struct amdrdna_display *display;
	struct thread *rest;
	uint32_t capabilities;
	bool wanted;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A8 begin");
	node = device->node;
	display = &device->display;

	/* With amdrdna.disp.own=0 stage A5s registered the device without a display. */
	wanted = amdrdna_display_wanted();
	if (!wanted) {
		amdrdna_mark("A8 skip: the display stays the firmware's (amdrdna.disp.own=0)");
		return 0;
	}

	/* The device stage A5s made, and the display and cursor stages A6 and A7 took over. */
	if (node == NULL || !display->taken || !display->cursor_ready) {
		amdrdna_mark("A8 fail: no gpu device, display or cursor (stages A5s, A6, A7)");
		return ENODEV;
	}

	/* The display is added before the one registration. */
	if (node->gpu != NULL) {
		amdrdna_mark("A8 fail: the gpu device was registered without its display");
		return EBUSY;
	}

	/* The lease's locks and state: no lease, leases numbered from one. */
	(void)mutex_init(&node->present_lock, LOCK_RANK_DEVICE, "amdrdna present");
	(void)mutex_init(&node->display_lock, LOCK_RANK_DEVICE, "amdrdna display");
	node->owner = NULL;
	node->lease = 0U;
	node->next_lease = 1U;
	node->sequence = 0U;
	node->shown = NULL;
	node->next_frame = 0U;
	node->text_held = 0;
	node->console_black = 0;
	node->console_due = 0U;

	/* The display's and the scanout's operations and the capabilities they make good. */
	capabilities = node->ops.capabilities;
	node->ops.display = &present_display_ops;
	node->ops.scanout = &present_scanout_ops;
	node->ops.capabilities |= GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS | GPU_CAP_DISPLAY_CONTROL;
	node->display = 1;

	/* Registers the device; the core publishes /dev/gpuN. */
	error = amdrdna_node_register(device);
	if (error != 0) {
		node->display = 0;
		node->ops.capabilities = capabilities;
		node->ops.display = NULL;
		node->ops.scanout = NULL;
		amdrdna_mark("A8 fail: gpu device not registered (%d)", error);
		return error;
	}

	/*
	 * The thread that lets the console draw again after a rest without a
	 * lease.  Without it the console stays stopped from the first frame on
	 * until the stop; the display works the same.
	 */
	error = kthread_create(present_rest_watch, node, SCHED_PRIORITY_DEFAULT, &rest);
	if (error == 0) {
		rest->detached = 1;
		thread_start(rest);
	} else {
		amdrdna_mark("A8: no console rest thread (%d); the console stays stopped after the first frame", error);
	}

	/* Marks the result. */
	amdrdna_mark("A8 ok gpu device registered, display %ux%u@%u.%03uHz stride=%u cursor %ux%u renderer=%d",
	    (unsigned)display->width,
	    (unsigned)display->height,
	    (unsigned)(display->refresh_millihz / 1000U),
	    (unsigned)(display->refresh_millihz % 1000U),
	    (unsigned)display->stride,
	    PRESENT_CURSOR_SIDE,
	    PRESENT_CURSOR_SIDE,
	    node->renderer);

	/* Succeeded: the GPU device has the display. */
	return 0;
}

/* Ends a closing session's lease, if it holds it. */
void
amdrdna_present_session_close(
	struct amdrdna_node *node,
	void *session)
{
	/* A device without a display holds no lease. */
	if (node->display == 0)
		return;

	/* The lease ends when the session held it, after a presentation still running. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->display_lock);

	if (node->owner == session)
		present_end_lease(node);

	mutex_unlock(&node->display_lock);
	mutex_unlock(&node->present_lock);
}

/*
 * Takes a blob that is going off the screen: the console comes back
 * first.  A blob the plane may still read is marked kept.
 */
void
amdrdna_present_resource_gone(
	struct amdrdna_node *node,
	struct amdrdna_resource *resource)
{
	int error;

	/* A device without a display shows no blob. */
	if (node->display == 0)
		return;

	/* Flips to the console when the blob is the one shown, after a presentation still running. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->display_lock);

	if (node->shown == resource) {
		error = EIO;
		if (node->broken == 0 && node->stopped == 0)
			error = amdrdna_display_show(node->device, node->device->display.console);
		if (error != 0) {
			present_broken(node, error);
			amdrdna_backing_keep(node->device, resource->backing);
		}

		/* No blob is on the screen any more. */
		node->shown = NULL;
	}

	mutex_unlock(&node->display_lock);
	mutex_unlock(&node->present_lock);
}

/*
 * Ends the lease for the reboot, whoever holds it, and keeps every later
 * operation off the display's registers: the console's surface is on the
 * plane when the display is given back to the firmware.
 */
void
amdrdna_present_stop(
	struct amdrdna_node *node)
{
	/* A device without a display has nothing to stop. */
	if (node->display == 0)
		return;

	/* The lease ends as if its holder had released it; nothing touches the display after. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->display_lock);

	if (node->owner != NULL)
		present_end_lease(node);

	/* The console's surface is on the plane: the console draws again, for the reboot's lines. */
	present_text_give_back(node);
	node->stopped = 1;

	mutex_unlock(&node->display_lock);
	mutex_unlock(&node->present_lock);
}

/* Describes the one display (the display query operation). */
static int
present_query(
	void *private_data,
	void *session,
	struct gpu_display_info *request)
{
	const struct amdrdna_sink *sink;
	struct amdrdna_node *node;
	struct amdrdna_display *display;
	bool connected;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;
	display = &node->device->display;

	/* One display exists. */
	request->count = 1U;

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Only display 0 exists. */
	if (request->index != 0U)
		return EINVAL;

	/* The display: FIFO, showing blobs, counting its refreshes, powered by its lease, with a cursor. */
	request->display_id = PRESENT_DISPLAY_ID;
	request->generation = PRESENT_GENERATION;
	request->flags = GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB | GPU_DISPLAY_REFRESH_COUNTER | GPU_DISPLAY_POWER_CONTROL | GPU_DISPLAY_HAS_CURSOR;

	/* Connected while the monitor is. */
	(void)amdrdna_display_topology(node->device, &connected);
	if (connected)
		request->flags |= GPU_DISPLAY_CONNECTED;

	/*
	 * Active while a lease has frames on it, powered off while its holder
	 * asked; the mode and the sink under the display lock (a mode set and
	 * a monitor's plug change them under it).
	 */
	mutex_lock(&node->display_lock);

	if (node->owner != NULL && node->sequence != 0U)
		request->flags |= GPU_DISPLAY_ACTIVE;
	if (node->powered_off != 0)
		request->flags |= GPU_DISPLAY_POWERED_OFF;

	/* One plane, the one packed format, and the largest frame: the firmware's surface. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888;
	request->max_frame_bytes = display->frame_bytes;

	/* The mode shown now; the largest is the frames' (the firmware's mode). */
	request->current_width = display->width;
	request->current_height = display->height;
	request->preferred_width = display->width;
	request->preferred_height = display->height;
	request->max_width = node->device->gop.viewport_width;
	request->max_height = node->device->gop.viewport_height;
	request->refresh_millihz = display->refresh_millihz;

	/* The sink's size, name and preferred mode when stage A10 read its EDID. */
	request->physical_width_mm = 0U;
	request->physical_height_mm = 0U;
	kern_memcpy(request->name, "amdrdna output", sizeof("amdrdna output"));
	sink = amdrdna_sink_ready(node->device);
	if (sink != NULL) {
		request->physical_width_mm = sink->edid.width_mm;
		request->physical_height_mm = sink->edid.height_mm;
		if (sink->edid.mode_count != 0U && sink->edid.modes[0].source == AMDRDNA_TIMING_PREFERRED) {
			request->physical_width_mm = sink->edid.modes[0].width_mm;
			request->physical_height_mm = sink->edid.modes[0].height_mm;
			request->preferred_width = sink->edid.modes[0].width;
			request->preferred_height = sink->edid.modes[0].height;
		}
		if (sink->edid.name[0] != '\0')
			kern_strcpy(request->name, sink->edid.name);
	}

	mutex_unlock(&node->display_lock);

	/* Succeeded: the display is described. */
	return 0;
}

/*
 * Enumerates or checks the modes (the display mode operation): the
 * firmware's, then those the sink's EDID lists that the output carries
 * now (stage A10).  Nothing is set.
 */
static int
present_mode(
	void *private_data,
	void *session,
	struct gpu_display_mode *request)
{
	struct amdrdna_node *node;
	uint32_t refresh;
	unsigned count;
	bool found;
	int error;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;

	/* Only the one display of this generation exists. */
	if (request->display_id != PRESENT_DISPLAY_ID)
		return ENOENT;
	if (request->generation != PRESENT_GENERATION)
		return ESTALE;

	/* Enumeration: the index-th mode, or the count (the mode and the sink under the display lock). */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		mutex_lock(&node->display_lock);

		found = amdrdna_sink_mode(node->device, request->index, &request->width, &request->height, &request->refresh_millihz,
		    &count);

		mutex_unlock(&node->display_lock);

		request->count = count;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (!found)
			return EINVAL;
		return 0;
	}

	/* Validation: a mode offered (0 asks for its preferred refresh); one the output does not show is ERANGE. */
	refresh = request->refresh_millihz;
	mutex_lock(&node->display_lock);

	error = amdrdna_sink_check(node->device, request->width, request->height, &refresh);

	mutex_unlock(&node->display_lock);

	if (error != 0)
		return error;

	/* Succeeded: the mode can be lit, at the refresh given back. */
	request->refresh_millihz = refresh;
	return 0;
}

/* Gives the display's lease to a session (the display claim operation); EBUSY while another holds it. */
static int
present_claim(
	void *private_data,
	void *session,
	struct gpu_display_claim *request)
{
	struct amdrdna_node *node;

	/* The node and what the core passed in. */
	node = private_data;

	/* Only plane 0 of the one display of this generation exists. */
	if (request->display_id != PRESENT_DISPLAY_ID)
		return ENOENT;
	if (request->generation != PRESENT_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

	/* Takes the lease when it is free and the display works. */
	mutex_lock(&node->display_lock);

	if (node->broken != 0 || node->stopped != 0) {
		mutex_unlock(&node->display_lock);
		return EIO;
	}

	/* Another session holds the lease. */
	if (node->owner != NULL) {
		mutex_unlock(&node->display_lock);
		return EBUSY;
	}

	/* The session holds a new lease with no frame yet; the console stays until the first. */
	node->owner = session;
	node->lease = node->next_lease;
	node->next_lease++;
	node->sequence = 0U;
	node->leases++;
	request->lease = node->lease;

	mutex_unlock(&node->display_lock);

	/* Succeeded: the session holds the display. */
	amdrdna_mark("gpu device: display lease %llu claimed", (unsigned long long)request->lease);
	return 0;
}

/* Ends the lease of its holder (the display release operation); EINVAL for another session. */
static int
present_release(
	void *private_data,
	void *session,
	const struct gpu_display_release *request)
{
	struct amdrdna_node *node;

	/* The node and what the core passed in. */
	node = private_data;

	/* Ends the lease, bringing the console's surface back, after a presentation still running. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->display_lock);

	if (node->owner != session || node->lease != request->lease) {
		mutex_unlock(&node->display_lock);
		mutex_unlock(&node->present_lock);
		return EINVAL;
	}

	/* The lease ends. */
	present_end_lease(node);

	mutex_unlock(&node->display_lock);
	mutex_unlock(&node->present_lock);

	/* Succeeded: the lease is free. */
	return 0;
}

/*
 * Shows a frame of the lease holder (the display present operation).
 *
 * A blob is flipped to as it is: its rows the display's, its image on a
 * 256-byte boundary of the VRAM.  Storage is copied into the display's
 * frame not on the screen, which is flipped to.  A frame of another size
 * than the mode shown sets a mode of its size first (one the display
 * offers, at the refresh asked for); the call returns once the flip has
 * begun.  Returns 0 with the frame's sequence, EINVAL for another
 * session's lease or a frame the display cannot show, ESTALE for another
 * generation, or EIO when the mode set failed or the flip did not begin
 * (the display is then broken).
 */
static int
present_present(
	void *private_data,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct amdrdna_node *node;
	struct amdrdna_resource *resource;
	struct amdrdna_device *device;
	uint64_t address;
	uint64_t times[PRESENT_TIMES];
	bool blob;
	bool resize;
	bool held;
	int error;

	/* The node and what the core passed in, and when the call began. */
	times[0] = sched_ticks();
	node = private_data;
	resource = object;
	device = node->device;
	held = false;

	/* A blob is flipped to as it is; storage is copied first. */
	blob = false;
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U)
		blob = true;

	/* The frame has the display's format. */
	if (request->format != GPU_PIXEL_BGRA8888) {
		amdrdna_mark("present refused: format %u", (unsigned)request->format);
		return EINVAL;
	}

	/* One presentation at a time; the lease cannot end while it runs. */
	mutex_lock(&node->present_lock);
	times[1] = sched_ticks();

	/*
	 * Checks the lease and picks the surface under the display lock; a
	 * frame of another size sets a mode of its size first.
	 */
	mutex_lock(&node->display_lock);

	error = present_admit(node, session, resource, request, blob, &address);
	resize = false;
	if (error == 0) {
		if (request->width != device->display.width || request->height != device->display.height)
			resize = true;
	}
	if (resize)
		error = present_resize(node, request);

	mutex_unlock(&node->display_lock);

	/* Reports a frame the lease or the display does not take. */
	if (error != 0) {
		mutex_unlock(&node->present_lock);
		amdrdna_mark("present refused (%d): blob %u stride %u offset %llu %ux%u", error, (unsigned)blob, (unsigned)request->stride,
			  (unsigned long long)request->offset, (unsigned)request->width, (unsigned)request->height);
		return error;
	}

	/* Storage goes into the frame not on the screen, which only presentations touch. */
	if (!blob)
		present_copy(node, resource, request);

	/* Writes the flip under the display lock: the cursor's writes and the flip take turns. */
	times[2] = sched_ticks();
	mutex_lock(&node->display_lock);

	error = amdrdna_display_flip(device, address);

	mutex_unlock(&node->display_lock);

	/* Waits for the frame's update that takes the flip, outside the display lock. */
	times[3] = sched_ticks();
	if (error == 0)
		error = amdrdna_display_flip_wait(device, address);
	times[4] = sched_ticks();
	present_slow_mark(node, times);

	/* Records the frame on the screen, or the broken display. */
	mutex_lock(&node->display_lock);

	if (error == 0) {
		/* The blob on the screen, or the other frame for the next copy. */
		node->shown = NULL;
		if (blob) {
			node->shown = resource;
		} else {
			node->next_frame ^= 1U;
		}

		/* The frame counts: its number and the time it was shown. */
		node->sequence++;
		node->presents++;
		node->shown_time_ns = kern_ticks_to_ns(sched_ticks());
		request->sequence = node->sequence;

		/* The console's surface is off the screen: its text stops drawing there. */
		held = present_text_hold(node);
	} else {
		present_broken(node, error);
		if (blob)
			node->shown = resource;
	}

	mutex_unlock(&node->display_lock);

	/* The console just stopped: its surface, off the screen, goes black before the lease can end. */
	if (held)
		present_console_paint(node);

	mutex_unlock(&node->present_lock);

	/* Reports a flip that did not begin. */
	if (error != 0)
		return EIO;

	/* Succeeded: the frame is on the screen. */
	return 0;
}

/*
 * Reports the newest frame shown under the lease (the display wait
 * operation): a presentation returns once shown, so this answers at once.
 * Returns EBUSY for another session's lease, EINVAL for a sequence not
 * presented yet, EAGAIN before the first frame.
 */
static int
present_wait(
	void *private_data,
	void *session,
	struct gpu_display_wait *request)
{
	struct amdrdna_node *node;

	/* The node and what the core passed in. */
	node = private_data;

	/* Reads the lease's newest frame under the display lock. */
	mutex_lock(&node->display_lock);

	if (node->owner != session || node->lease != request->lease) {
		mutex_unlock(&node->display_lock);
		return EBUSY;
	}

	/* A sequence not presented yet. */
	if (request->sequence > node->sequence) {
		mutex_unlock(&node->display_lock);
		return EINVAL;
	}

	/* No frame yet. */
	if (node->sequence == 0U) {
		mutex_unlock(&node->display_lock);
		return EAGAIN;
	}

	/* The newest frame and its time. */
	request->completed_sequence = node->sequence;
	request->present_time_ns = node->shown_time_ns;
	request->generation = PRESENT_GENERATION;

	mutex_unlock(&node->display_lock);

	/* Succeeded: the sequence has been shown. */
	return 0;
}

/*
 * Reports the display's topology sequence (the display events operation):
 * one at first, one more for every plug or unplug of the monitor.  Any
 * context: only the display's IRQ-safe lock, no hardware access, no wait.
 */
static int
present_events(
	void *private_data,
	void *session,
	uint64_t *sequence)
{
	struct amdrdna_node *node;
	bool connected;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;

	/* The sequence the interrupt thread moves. */
	*sequence = amdrdna_display_topology(node->device, &connected);

	/* Succeeded: the topology is reported. */
	return 0;
}

/*
 * Powers the display on or off for the lease's holder (the display power
 * operation): off blanks the plane, so the output shows black, and makes
 * no refresh boundary for the lease's waits; on shows the plane again.
 * Returns ENOENT for another display, ESTALE for another generation, EBUSY
 * when this open does not hold the lease, EIO for a broken display.
 */
static int
present_power(
	void *private_data,
	void *session,
	const struct gpu_display_power *request)
{
	struct amdrdna_node *node;
	int error;

	/* The node and what the core passed in. */
	node = private_data;

	/* Only the one display of this generation exists. */
	if (request->display_id != PRESENT_DISPLAY_ID)
		return ENOENT;
	if (request->generation != PRESENT_GENERATION)
		return ESTALE;

	/* Blanks or shows the plane for the lease's holder. */
	mutex_lock(&node->display_lock);

	if (node->owner != session) {
		mutex_unlock(&node->display_lock);
		return EBUSY;
	}

	/* A broken or stopped display takes no change. */
	if (node->broken != 0 || node->stopped != 0) {
		mutex_unlock(&node->display_lock);
		return EIO;
	}

	/* On shows the plane; off and suspend blank it. */
	if (request->state == GPU_DISPLAY_POWER_ON) {
		error = amdrdna_display_blank(node->device, false);
		node->powered_off = 0;
	} else {
		error = amdrdna_display_blank(node->device, true);
		node->powered_off = 1;
	}

	mutex_unlock(&node->display_lock);

	/* Reports a plane that did not change. */
	if (error != 0)
		return EIO;

	/* Succeeded: the display is on or off. */
	return 0;
}

/*
 * Waits for the display's refresh boundary after the request's cursor, or
 * reports the count at once for cursor 0 (the display refresh operation).
 *
 * The boundaries are the timing generator's frames.  A display powered
 * off makes none.  Returns 0 with sequence and time_ns, ENOENT for another
 * display, ESTALE for another generation, or ETIMEDOUT.
 */
static int
present_refresh(
	void *private_data,
	void *session,
	struct gpu_display_refresh *request)
{
	struct amdrdna_node *node;
	uint64_t timeout_ns;
	uint64_t deadline;
	uint64_t count;
	uint64_t time_ns;
	uint64_t now;
	int powered_off;
	int error;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;

	/* Only the one display of this generation exists. */
	if (request->display_id != PRESENT_DISPLAY_ID)
		return ENOENT;
	if (request->generation != PRESENT_GENERATION)
		return ESTALE;

	/* The deadline in scheduler ticks, rounded up, at most a second away. */
	timeout_ns = request->timeout_ns;
	if (timeout_ns > PRESENT_REFRESH_MAX_NS)
		timeout_ns = PRESENT_REFRESH_MAX_NS;
	deadline = sched_ticks() + kern_ns_to_ticks(timeout_ns);

	/* Whether the lease's holder powered the display off. */
	mutex_lock(&node->display_lock);

	powered_off = node->powered_off;

	mutex_unlock(&node->display_lock);

	/* A display powered off makes no boundary after the cursor: the wait runs out. */
	if (powered_off != 0 && request->cursor != 0U) {
		now = sched_ticks();
		while (now < deadline) {
			sched_sleep(deadline);
			now = sched_ticks();
		}

		/* No boundary came. */
		return ETIMEDOUT;
	}

	/* The boundary after the cursor, or the count now. */
	error = amdrdna_display_refresh_wait(node->device, request->cursor, deadline, &count, &time_ns);
	if (error != 0)
		return error;

	/* Succeeded: the boundary after the cursor. */
	request->sequence = count;
	request->time_ns = time_ns;
	request->flags = 0U;
	return 0;
}

/*
 * Sets the cursor's image from the lease holder's storage (the display
 * cursor set operation): the rows are copied at once into the slot the
 * cursor does not read, as A8R8G8B8, the rest of the 64 by 64 slot
 * transparent, and the cursor is pointed at it.  A shown cursor shows it
 * from the next frame's update; a hidden one stays hidden.
 *
 * Returns 0, EBUSY for another open's lease, ESTALE for another
 * generation, EINVAL for an image larger than 64 by 64, or EIO.
 */
static int
present_cursor_set(
	void *private_data,
	void *session,
	void *object,
	const struct gpu_display_cursor_image *request)
{
	struct amdrdna_node *node;
	const struct amdrdna_resource *resource;
	unsigned slot;
	int error;

	/* The node and what the core passed in. */
	node = private_data;
	resource = object;

	/* The image fits the slot. */
	if (request->width == 0U || request->width > PRESENT_CURSOR_SIDE)
		return EINVAL;
	if (request->height == 0U || request->height > PRESENT_CURSOR_SIDE)
		return EINVAL;

	/* A blob outside the CPU's window is read in whole words. */
	if (resource->kind != AMDRDNA_RESOURCE_STORAGE && !resource->backing->cpu &&
	    ((request->offset % PRESENT_PIXEL_BYTES) != 0U || (request->stride % PRESENT_PIXEL_BYTES) != 0U))
		return EINVAL;

	/* Checks the lease and changes the cursor under the display lock. */
	mutex_lock(&node->display_lock);

	error = present_cursor_check(node, session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&node->display_lock);
		return error;
	}

	/* Copies the image into the slot the cursor does not read, then points the cursor at it. */
	slot = node->device->display.cursor_slot ^ 1U;
	present_cursor_copy(node, resource, request, slot);
	amdrdna_host_path_flush(node->device);
	amdrdna_display_cursor_use(node->device, slot);
	node->cursor_has_image = 1;

	mutex_unlock(&node->display_lock);

	/* Succeeded: the image is taken; the storage is no longer read. */
	return 0;
}

/*
 * Moves the cursor's top left to a position of the output (the display
 * cursor move operation), showing a hidden cursor; an image wholly off the
 * output hides it.
 *
 * Returns 0, EINVAL when the lease set no image, EBUSY for another open's
 * lease, ESTALE for another generation, or EIO.
 */
static int
present_cursor_move(
	void *private_data,
	void *session,
	const struct gpu_display_cursor_move *request)
{
	struct amdrdna_node *node;
	int error;

	/* The node and what the core passed in. */
	node = private_data;

	/* Checks the lease and moves the cursor under the display lock. */
	mutex_lock(&node->display_lock);

	error = present_cursor_check(node, session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&node->display_lock);
		return error;
	}

	/* Only an image the lease set can be shown. */
	if (node->cursor_has_image == 0) {
		mutex_unlock(&node->display_lock);
		return EINVAL;
	}

	/* The position, from the next frame's update on. */
	amdrdna_display_cursor_place(node->device, request->x, request->y);
	node->cursor_x = request->x;
	node->cursor_y = request->y;
	node->cursor_visible = 1;

	mutex_unlock(&node->display_lock);

	/* Succeeded: the cursor is at the position from a coming frame on. */
	return 0;
}

/*
 * Hides the cursor and keeps its image (the display cursor hide
 * operation).  Returns 0 (also for a cursor already hidden), EBUSY for
 * another open's lease, ESTALE for another generation, or EIO.
 */
static int
present_cursor_hide(
	void *private_data,
	void *session,
	const struct gpu_display_cursor_hide *request)
{
	struct amdrdna_node *node;
	int error;

	/* The node and what the core passed in. */
	node = private_data;

	/* Checks the lease and hides the cursor under the display lock. */
	mutex_lock(&node->display_lock);

	error = present_cursor_check(node, session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&node->display_lock);
		return error;
	}

	/* Hides it; the image stays for a later move. */
	amdrdna_display_cursor_hide(node->device);
	node->cursor_visible = 0;

	mutex_unlock(&node->display_lock);

	/* Succeeded: the cursor is gone from a coming frame on. */
	return 0;
}

/*
 * Answers the pairing query: the device shows images, and renders them
 * too when the renderer is bound (not amdrdna.vk=0); it has no companion.
 */
static int
present_query_device(
	void *private_data,
	void *session,
	struct gpu_device_info *request)
{
	struct amdrdna_node *node;

	UNUSED_PARAMETER(session);

	/* The node and what the core passed in. */
	node = private_data;

	/* A display, and a renderer when bound; no preferred other device. */
	request->roles = GPU_DEVICE_DISPLAY;
	if (node->renderer != 0)
		request->roles |= GPU_DEVICE_RENDER;
	request->companion_id = 0U;

	/* Succeeded: the roles are filled in. */
	return 0;
}

/*
 * Answers the scanout constraints of the one display: shared (a blob of
 * this device) or copied (storage), BGRA, rows a multiple of 256 bytes
 * apart from a 256-byte boundary (and a shared image's rows must be the
 * display's), no placement condition (every blob is VRAM the plane
 * reads).  Returns 0, ENOENT for another display or ESTALE for another
 * generation.
 */
static int
present_constraints(
	void *private_data,
	void *session,
	struct gpu_scanout_constraints *request)
{
	UNUSED_PARAMETER(private_data);
	UNUSED_PARAMETER(session);

	/* Only the one display of this generation exists. */
	if (request->display_id != PRESENT_DISPLAY_ID)
		return ENOENT;
	if (request->generation != PRESENT_GENERATION)
		return ESTALE;

	/* A blob of this device is shown as it is; storage is copied by the CPU. */
	request->flags = GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888;

	/* The plane's linear surface. */
	request->stride_alignment = PRESENT_STRIDE_ALIGN;
	request->offset_alignment = PRESENT_OFFSET_ALIGN;
	request->placement = 0U;
	request->max_dma_address = 0U;

	/* Succeeded: the constraints are filled in. */
	return 0;
}

/*
 * Checks a presentation against the lease and the display and finds the
 * GPU address of the surface it flips to: the blob itself, or the
 * display's frame not on the screen, which the storage is to be copied
 * into.  The caller holds the node's present lock and its display lock.
 * Returns 0, EINVAL, ESTALE or EIO as present_present() does.
 */
static int
present_admit(
	struct amdrdna_node *node,
	void *session,
	const struct amdrdna_resource *resource,
	const struct gpu_display_present *request,
	bool blob,
	uint64_t *address)
{
	struct amdrdna_display *display;
	uint64_t vram;

	/* Only the lease's holder presents. */
	display = &node->device->display;
	if (node->owner != session || node->lease != request->lease)
		return EINVAL;

	/* Only the one generation exists. */
	if (request->generation != PRESENT_GENERATION)
		return ESTALE;

	/* A broken or stopped display flips nothing more. */
	if (node->broken != 0 || node->stopped != 0)
		return EIO;

	/* A storage frame goes into the frame not on the screen. */
	if (!blob) {
		if (resource->kind != AMDRDNA_RESOURCE_STORAGE || (request->offset % PRESENT_PIXEL_BYTES) != 0U)
			return EINVAL;
		*address = amdrdna_display_address(node->device, display->frames[node->next_frame]);
		return 0;
	}

	/* A blob is the surface itself: VRAM, its image on 256 bytes, its rows the display's. */
	if (resource->kind != AMDRDNA_RESOURCE_BLOB)
		return EINVAL;
	vram = resource->backing->vram + request->offset;
	if ((vram % PRESENT_OFFSET_ALIGN) != 0U || request->stride != display->stride)
		return EINVAL;

	/* Succeeded: the frame may be shown. */
	*address = amdrdna_display_address(node->device, vram);
	return 0;
}

/*
 * Copies a storage frame's rows into the display's frame not on the
 * screen through the CPU's window, then pushes them past the host path.
 * The caller checked the frame has the display's size; the core checked
 * its rows lie inside the storage.
 */
static void
present_copy(
	struct amdrdna_node *node,
	const struct amdrdna_resource *resource,
	const struct gpu_display_present *request)
{
	struct amdrdna_display *display;
	volatile uint8_t *target_base;
	volatile uint32_t *target;
	const uint32_t *source;
	uint32_t row;
	uint32_t column;

	/* The frame the copy goes to is the one not on the screen. */
	display = &node->device->display;
	target_base = amdrdna_vram_cpu(node->device, display->frames[node->next_frame]);

	/* Copies each row word by word. */
	for (row = 0U; row < request->height; row++) {
		source = (const uint32_t *)(const void *)(resource->cpu + request->offset + (uint64_t)row * request->stride);
		target = (volatile uint32_t *)(volatile void *)(target_base + (uint64_t)row * display->stride);
		for (column = 0U; column < request->width; column++)
			target[column] = source[column];
	}

	/* The plane reads what was written, not what the host path holds. */
	amdrdna_host_path_flush(node->device);
}

/*
 * Ends the lease: the cursor goes, image and all, a powered-off plane is
 * shown again, the console's surface (black, the console still stopped)
 * comes back when a frame of the lease is up, and the blob on the screen
 * is let go (kept when the flip back failed).  The console's rest starts.
 * The caller holds the node's present lock and its display lock.
 */
static void
present_end_lease(
	struct amdrdna_node *node)
{
	struct amdrdna_device *device;
	int error;

	/* The lease's cursor goes. */
	device = node->device;
	if (node->stopped == 0) {
		amdrdna_display_cursor_hide(device);
		if (node->powered_off != 0)
			(void)amdrdna_display_blank(device, false);
	}

	/* The next lease starts without an image and powered on. */
	node->cursor_has_image = 0;
	node->cursor_visible = 0;
	node->powered_off = 0;

	/* Flips back to the console's surface, black while the console is stopped, when a frame of the lease is up. */
	if (node->sequence != 0U && node->broken == 0 && node->stopped == 0) {
		if (node->console_black == 0)
			amdrdna_mark("gpu device: the console's surface is shown without its paint");
		error = amdrdna_display_show(device, device->display.console);
		if (error != 0)
			present_broken(node, error);

		/* A blob the plane read is let go once the console is back. */
		if (error == 0)
			node->shown = NULL;
	}

	/* The console's mode, the firmware's, when the lease set another. */
	if (node->broken == 0 && node->stopped == 0)
		present_firmware_mode(node);

	/* A broken display keeps the blob it may still read. */
	if (node->shown != NULL) {
		amdrdna_backing_keep(device, node->shown->backing);
		node->shown = NULL;
	}

	/* The console stays stopped for the next lease; the rest thread lets it draw after a rest. */
	if (node->text_held != 0)
		node->console_due = sched_ticks() + kern_ms_to_ticks(PRESENT_CONSOLE_REST_MS);

	/* The lease is free. */
	amdrdna_mark("gpu device: display lease %llu released after %llu frames", (unsigned long long)node->lease, (unsigned long long)node->sequence);
	node->owner = NULL;
	node->lease = 0U;
	node->sequence = 0U;
}

/*
 * Sets the mode of a frame's size before it is shown: the firmware's, or
 * a mode the sink lists that the output shows, at the refresh asked for
 * (0 the first of that size).  The caller holds the node's present lock
 * and its display lock.
 *
 * Returns 0; EINVAL for a size the display does not offer or a mode the
 * board cannot show; EIO when the mode set failed (the timing generator
 * then runs the new mode, which frames of its size are still shown in).
 */
static int
present_resize(
	struct amdrdna_node *node,
	const struct gpu_display_present *request)
{
	struct amdrdna_device *device;
	struct amdrdna_timing timing;
	int error;

	/* The mode of that size. */
	device = node->device;
	error = amdrdna_sink_choose(device, request->width, request->height, request->refresh_millihz, &timing);
	if (error != 0)
		return EINVAL;

	/* The mode set, in the display's order. */
	mutex_lock(&device->display.mode_lock);

	error = amdrdna_mode_set(device, &timing);

	mutex_unlock(&device->display.mode_lock);

	/* Reports a mode the board cannot show, or a set that failed. */
	if (error == ERANGE ||
	    error == ENOENT ||
	    error == ENOTSUP)
		return EINVAL;
	if (error != 0)
		return EIO;

	/* Succeeded: the frame's size is the display's. */
	return 0;
}

/*
 * Sets the firmware's mode again when a lease set another, so the console
 * is shown as the firmware lit it.  The caller holds the node's present
 * lock and its display lock.  A failed set is said and left: the display
 * keeps running in the mode the set left.
 */
static void
present_firmware_mode(
	struct amdrdna_node *node)
{
	struct amdrdna_display *display;
	int error;

	/* Only after a mode set of the lease. */
	display = &node->device->display;
	mutex_lock(&display->mode_lock);

	error = 0;
	if (display->mode_known) {
		if (display->mode.width != display->firmware_mode.width ||
		    display->mode.height != display->firmware_mode.height ||
		    display->mode.pixel_khz != display->firmware_mode.pixel_khz)
			error = amdrdna_mode_set(node->device, &display->firmware_mode);
	}

	mutex_unlock(&display->mode_lock);

	/* Says a set that failed. */
	if (error != 0)
		amdrdna_mark("gpu device: the firmware's mode was not set again (%d)", error);
}

/*
 * Marks the display broken after a flip that did not begin: nothing is
 * flipped after it.  The caller holds the node's display lock.
 */
static void
present_broken(
	struct amdrdna_node *node,
	int error)
{
	/* Presentations, the cursor and the lease's end leave the plane alone from now on. */
	node->broken = 1;
	amdrdna_mark("gpu device: a flip did not begin (%d); the display stops", error);
}

/*
 * Keeps the kernel's text console from drawing while frames of leases are
 * on the screen, from the first on.  The caller holds the node's display
 * lock.
 *
 * Returns true when the console stopped here (its surface is to be
 * painted), false when it was stopped already.
 */
static bool
present_text_hold(
	struct amdrdna_node *node)
{
	/* The rest, if one ran, is over: a frame is up again. */
	node->console_due = 0U;

	/* Held already since an earlier frame. */
	if (node->text_held != 0)
		return false;

	/*
	 * text_held tells the rest and the stop that the console was stopped
	 * here and is to draw again.
	 */
	kern_text_suspend();
	node->text_held = 1;
	return true;
}

/*
 * Lets the kernel's text console draw again at the lease's end, repainting
 * what it kept.  The caller holds the node's display lock.
 */
static void
present_text_give_back(
	struct amdrdna_node *node)
{
	/* No frame of the lease stopped it. */
	if (node->text_held == 0)
		return;

	/* The console's surface is the one shown from now on: its text, drawn again, may cover the black. */
	node->text_held = 0;
	node->console_black = 0;
	node->console_due = 0U;
	kern_text_resume();
}

/*
 * Paints the console's surface black once the console stopped and the
 * plane reads a frame of the lease.  The caller holds the node's present
 * lock (no lease ends meanwhile) but not its display lock.
 */
static void
present_console_paint(
	struct amdrdna_node *node)
{
	int error;

	/* The paint, outside the display lock: the cursor goes on moving. */
	error = amdrdna_display_console_clear(node->device);
	if (error != 0)
		amdrdna_mark("gpu device: the console's surface was not painted (%d)", error);

	/* Says whether a flip back shows black. */
	mutex_lock(&node->display_lock);

	node->console_black = error == 0 ? 1 : 0;

	mutex_unlock(&node->display_lock);
}

/*
 * Lets the text console draw again once the display has gone
 * PRESENT_CONSOLE_REST_MS without a lease (the console's surface, black,
 * is on the plane since the last lease's end), looking every
 * PRESENT_REST_LOOK_MS until the stop.
 */
static void
present_rest_watch(
	void *argument)
{
	struct amdrdna_node *node;
	uint64_t now;
	bool stopped;
	bool due;

	/* The node the thread watches. */
	node = argument;

	/* One look a turn. */
	for (;;) {
		sched_sleep(sched_ticks() + kern_ms_to_ticks(PRESENT_REST_LOOK_MS));

		/* Whether the stop came, and whether the rest is over. */
		mutex_lock(&node->display_lock);

		now = sched_ticks();
		stopped = node->stopped != 0;
		due = node->owner == NULL && node->text_held != 0 && node->console_due != 0U && now >= node->console_due;

		mutex_unlock(&node->display_lock);

		/* The stop gave the console back already. */
		if (stopped)
			return;
		if (!due)
			continue;

		/* Checks again in the presentations' order, then lets the console draw. */
		mutex_lock(&node->present_lock);
		mutex_lock(&node->display_lock);

		now = sched_ticks();
		if (node->stopped == 0 && node->owner == NULL && node->text_held != 0 && node->console_due != 0U &&
		    now >= node->console_due) {
			present_text_give_back(node);
			amdrdna_mark("gpu device: the console draws again after %u ms without a lease", PRESENT_CONSOLE_REST_MS);
		}

		mutex_unlock(&node->display_lock);
		mutex_unlock(&node->present_lock);
	}
}

/*
 * Checks that the session holds the lease, of this generation, on a
 * display that works; the caller holds the node's display lock.
 */
static int
present_cursor_check(
	struct amdrdna_node *node,
	void *session,
	uint64_t lease,
	uint64_t generation)
{
	/* Only the lease's holder changes the cursor. */
	if (node->owner != session || node->lease != lease)
		return EBUSY;

	/* Only the one generation exists. */
	if (generation != PRESENT_GENERATION)
		return ESTALE;

	/* A broken or stopped display takes no change. */
	if (node->broken != 0 || node->stopped != 0)
		return EIO;

	/* The session may change the cursor. */
	return 0;
}

/*
 * Copies a cursor image's rows into one slot through the CPU's window, as
 * A8R8G8B8, with every pixel outside the image transparent.  The core
 * checked the rows lie inside the storage.
 */
static void
present_cursor_copy(
	struct amdrdna_node *node,
	const struct amdrdna_resource *resource,
	const struct gpu_display_cursor_image *request,
	unsigned slot)
{
	uint8_t line[PRESENT_CURSOR_SIDE * PRESENT_PIXEL_BYTES];
	const volatile uint8_t *source;
	const volatile uint8_t *row_source;
	const volatile uint8_t *pixel;
	volatile uint32_t *target;
	uint32_t value;
	uint32_t row;
	uint32_t column;
	bool outside;

	/*
	 * The image's first byte: in RAM for storage, in the window for a blob
	 * (its read cache dropped); a blob outside the window is read a row at
	 * a time through the indirect window.
	 */
	outside = false;
	source = NULL;
	if (resource->kind == AMDRDNA_RESOURCE_STORAGE) {
		source = resource->cpu + request->offset;
	} else if (resource->backing->cpu) {
		amdrdna_host_path_drop(node->device);
		source = amdrdna_backing_cpu(node->device, resource->backing) + request->offset;
	} else {
		outside = true;
	}

	/* The slot's pixels, 64 a row. */
	target = (volatile uint32_t *)(volatile void *)amdrdna_display_cursor_cpu(node->device, slot);

	/* Every pixel of the slot: the image's where it has one, transparent elsewhere. */
	for (row = 0U; row < PRESENT_CURSOR_SIDE; row++) {
		/* The row of the image, where it has one. */
		row_source = NULL;
		if (row < request->height && outside) {
			amdrdna_vram_read(node->device, resource->backing->vram + request->offset + (uint64_t)row * request->stride, line,
					  (size_t)request->width * PRESENT_PIXEL_BYTES);
			row_source = line;
		} else if (row < request->height) {
			row_source = source + (uint64_t)row * request->stride;
		}

		/* Each pixel of the row. */
		for (column = 0U; column < PRESENT_CURSOR_SIDE; column++) {
			/* Outside the image: transparent. */
			if (row >= request->height || column >= request->width) {
				target[row * PRESENT_CURSOR_SIDE + column] = 0U;
				continue;
			}

			/* The pixel's four bytes, B, G, R, A or R, G, B, A in memory. */
			pixel = row_source + (uint64_t)column * PRESENT_PIXEL_BYTES;
			if (request->format == GPU_PIXEL_RGBA8888) {
				value = (uint32_t)pixel[2] | ((uint32_t)pixel[1] << 8) | ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[3] << 24);
			} else {
				value = (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8) | ((uint32_t)pixel[2] << 16) | ((uint32_t)pixel[3] << 24);
			}

			/* The pixel in the slot. */
			target[row * PRESENT_CURSOR_SIDE + column] = value;
		}
	}
}

/* Marks a presentation that took PRESENT_SLOW_MS or more, with the time of each of its steps in ms. */
static void
present_slow_mark(
	struct amdrdna_node *node,
	const uint64_t *times)
{
	uint64_t total;

	/* Only a slow one, and only the first few. */
	total = kern_ticks_to_ms(times[PRESENT_TIMES - 1U] - times[0]);
	if (total < PRESENT_SLOW_MS)
		return;
	node->slow_presents++;
	if (node->slow_presents > PRESENT_SLOW_MARKS)
		return;

	/* The steps. */
	amdrdna_mark("present slow %llums (%llu so far): lock %llu, admit %llu, flip %llu, flip wait %llu",
	    (unsigned long long)total,
	    (unsigned long long)node->slow_presents,
	    (unsigned long long)kern_ticks_to_ms(times[1] - times[0]),
	    (unsigned long long)kern_ticks_to_ms(times[2] - times[1]),
	    (unsigned long long)kern_ticks_to_ms(times[3] - times[2]),
	    (unsigned long long)kern_ticks_to_ms(times[4] - times[3]));
}
