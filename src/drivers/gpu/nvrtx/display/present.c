/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display of the nvrtx GPU device (the gpu-device stage).
 *
 * One display: the head the firmware lit and the driver took over (takeover),
 * in the mode takeover set on it (the only mode); the console, when smaller, is enlarged to it by the head.  One lease at
 * a time, FIFO presentation of storage frames (copied by the CPU into one of
 * the node's two surfaces, then flipped to) and of blobs (flipped to as they
 * are).  A presentation returns once the window's flip has begun, so a wait
 * answers at once.  Releasing the lease, closing its session or destroying the
 * blob on the screen brings the console surface (takeover's first) back.
 *
 * The node's present lock puts the presentations, the lease's end and a
 * blob's going off the screen in one order; a presentation takes the
 * node's lock only to check the lease and to send its flip, and waits for
 * the head's refresh that begins the flip without it, so the renderer and
 * the cursor, which take the node's lock, are not held up by the display's
 * refresh.
 *
 * A flip that does not begin in time leaves the display broken: nothing is
 * flipped after it, presentations fail with EIO, and a blob that may still
 * be on the screen keeps its VRAM until power-off.
 *
 * The display's events are its topology sequence, which stays at one: the
 * resource manager's hotplug events are not taken yet.  Its control has
 * refresh boundaries (the head's frame counter, polled) but no power
 * control: switching the output off and on needs the resource manager's
 * modeset, which the device's operations do not call (EOPNOTSUPP).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/sched.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include "drivers/gpu/nvrtx/nvrtx-node.h"

/* How long a refresh wait sleeps before it looks again, in milliseconds (a tick at least). */
#define PRESENT_POLL_MS 1U

/* The largest frame a presentation takes. */
#define PRESENT_MAX_FRAME_BYTES		(64ULL << 20)

/*
 * A window reads a surface from a 256-byte boundary, with rows a multiple of
 * 64 bytes apart.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_image_set, the offset and the pitch the window is given).
 */
#define PRESENT_OFFSET_ALIGN		256U
#define PRESENT_PITCH_ALIGN		64U

/* The node's frames: rows on 256 bytes, each frame on its own 1 MiB step. */
#define PRESENT_FRAME_PITCH_ALIGN	256U
#define PRESENT_FRAME_ALIGN		0x100000ULL

/* The colour the node's frames start with (opaque black, A8R8G8B8). */
#define PRESENT_FRAME_CLEAR		0xff000000U

/*
 * The head's pixel clock: hertz in bits 30:0, bit 31 scales it by 1000/1001.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HERTZ,
 * NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_ADJ1000DIV1001).
 */
#define PRESENT_CLOCK_HERTZ_MASK	0x7fffffffU
#define PRESENT_CLOCK_ADJUST		0x80000000U

/* The refresh assumed when the mode has no raster or clock (60 Hz). */
#define PRESENT_REFRESH_DEFAULT		60000U

static int present_query(void *private_data, void *session, struct gpu_display_info *request);
static int present_mode(void *private_data, void *session, struct gpu_display_mode *request);
static int present_claim(void *private_data, void *session, struct gpu_display_claim *request);
static int present_release(void *private_data, void *session, const struct gpu_display_release *request);
static int present_present(void *private_data, void *session, void *object, struct gpu_display_present *request);
static int present_wait(void *private_data, void *session, struct gpu_display_wait *request);
static int present_events(void *private_data, void *session, uint64_t *sequence);
static int present_power(void *private_data, void *session, const struct gpu_display_power *request);
static int present_refresh_wait(void *private_data, void *session, struct gpu_display_refresh *request);

/*
 * The display operations of the nvrtx GPU device.
 *
 * Shared by the one node; immutable.  Query, mode, claim, release, present
 * and wait, the topology sequence, power and refresh (the display control)
 * are here, the cursor plane in cursor.c.  Presentations wait under the
 * open's admission (no ungated flag, no adoption wait), and there are no
 * overlay planes.
 */
const struct drv_gpu_display_ops nvrtx_display_ops = {
	present_query,
	present_mode,
	present_claim,
	present_release,
	present_present,
	present_wait,
	present_events,
	present_power,
	present_refresh_wait,
	nvrtx_cursor_set,
	nvrtx_cursor_move,
	nvrtx_cursor_hide,
	NULL,
	0U,
	NULL,
	NULL
};

static uint32_t present_refresh(const struct nvrtx_display_state *display);
static int present_admit(struct nvrtx_node *node, void *session, const struct nvrtx_resource *resource, const struct gpu_display_present *request, bool blob, struct nvrtx_disp_surface *surface);
static void present_shown(struct nvrtx_node *node, struct nvrtx_resource *resource, bool blob);
static int present_show(struct nvrtx_node *node, const struct nvrtx_disp_surface *surface);
static void present_broken(struct nvrtx_node *node, int error);
static void present_end_lease(struct nvrtx_node *node);
static void present_copy(struct nvrtx_node *node, const struct nvrtx_resource *resource, const struct gpu_display_present *request, const struct nvrtx_disp_surface *frame);
static uint32_t present_format(uint32_t format);

/*
 * Takes the node's two frames, measures the display's refresh and shows
 * the console surface.
 */
int
nvrtx_present_start(
	struct nvrtx_node *node)
{
	struct nvrtx_gpu *gpu;
	struct nvrtx_display_state *display;
	struct nvrtx_disp_surface *frame;
	volatile uint32_t *words;
	uint64_t span;
	uint64_t at;
	uint32_t pitch;
	unsigned index;
	int error;

	gpu = node->gpu;
	display = &gpu->display;

	/*
	 * The display's size is the mode's visible size, its refresh the mode's
	 * raster's.  The console surface may be smaller (the head's scaler
	 * enlarges it); the lease's frames have the mode's size, and the head's
	 * view follows each flip (nvrtx_scanout_frame).
	 */
	node->width = display->mode.active & 0xffffU;
	node->height = display->mode.active >> 16;
	node->refresh_millihz = present_refresh(display);

	/* Each frame has the display's size, rows on 256 bytes. */
	pitch = (node->width * 4U + PRESENT_FRAME_PITCH_ALIGN - 1U) & ~(PRESENT_FRAME_PITCH_ALIGN - 1U);
	span = (uint64_t)pitch * node->height;
	for (index = 0U; index < 2U; index++) {
		/* Takes the frame's VRAM and maps it for the CPU's copies. */
		error = nvrtx_bo_alloc(gpu, span, PRESENT_FRAME_ALIGN, &node->frame_objects[index]);
		if (error == 0)
			error = nvrtx_bo_map(gpu, &node->frame_objects[index]);
		if (error != 0)
			return error;

		/* Describes it. */
		frame = &node->frames[index];
		frame->offset = node->frame_objects[index].vram;
		frame->width = node->width;
		frame->height = node->height;
		frame->pitch = pitch;
		frame->handle = NVRTX_DISP_HANDLE_VRAM;
		frame->format = NVRTX_DISP_FORMAT_A8R8G8B8;

		/* Clears it to black. */
		words = (volatile uint32_t *)(volatile void *)nvrtx_bo_cpu(gpu, &node->frame_objects[index]);
		for (at = 0U; at < span / 4U; at++)
			words[at] = PRESENT_FRAME_CLEAR;
	}

	/* Shows the console surface; the first frame goes to the first of the node's. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->lock);

	node->next = 0U;
	error = present_show(node, &display->surfaces[0]);

	mutex_unlock(&node->lock);
	mutex_unlock(&node->present_lock);

	/* Reports a head that did not flip. */
	if (error != 0)
		return error;

	/* Succeeded: the display is ready for a lease. */
	return 0;
}

/*
 * Ends a closing session's lease, if it holds it.
 */
void
nvrtx_present_session_close(
	struct nvrtx_node *node,
	void *session)
{
	/* The console surface comes back when the session held the lease, after a presentation still running. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->lock);

	if (node->owner == session)
		present_end_lease(node);

	mutex_unlock(&node->lock);
	mutex_unlock(&node->present_lock);
}

/*
 * Takes a blob that is going off the screen: the console surface comes back
 * first.  A blob the display may still read is marked kept.
 */
void
nvrtx_present_resource_gone(
	struct nvrtx_node *node,
	struct nvrtx_resource *resource)
{
	int error;

	/* Flips to the console surface when the blob is the one shown, after a presentation still running (which may show it). */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->lock);

	if (node->shown == resource) {
		error = EIO;
		if (node->broken == 0)
			error = present_show(node, &node->gpu->display.surfaces[0]);
		if (error != 0)
			resource->backing->kept = 1;
		node->shown = NULL;
	}

	mutex_unlock(&node->lock);
	mutex_unlock(&node->present_lock);
}

/*
 * Ends the lease for the reboot, whoever holds it: the cursor is hidden
 * and the console surface (the firmware's picture) comes back on the head,
 * so the screen is as the firmware left it when the GSP is stopped.  The
 * caller holds the node's present lock and its lock.
 */
void
nvrtx_present_stop(
	struct nvrtx_node *node)
{
	/* Without a lease the console surface is on the head already. */
	if (node->owner == NULL)
		return;

	/* The lease ends as if its holder had released it. */
	present_end_lease(node);
}

/*
 * Brings the refresh count up to date from the head's frame counter and
 * reports it with the time it last moved.
 *
 * The 16-bit counter's movement since the last reading is added, so the
 * count never goes back; a gap of more than 65535 frames between readings
 * counts short (the message drain reads it about each second).  The first
 * reading only sets the base.  Takes only the IRQ-safe refresh lock.
 */
void
nvrtx_present_refresh_sample(
	struct nvrtx_node *node,
	uint64_t *count,
	uint64_t *time_ns)
{
	unsigned long irq;
	uint32_t frame;
	uint32_t advance;

	/* Reads the head's frame counter. */
	irq = spin_lock_irqsave(&node->refresh_lock);

	frame = nvrtx_scanout_frame_counter(node->gpu);

	/* Adds what it moved since the last reading; the first reading only sets the base. */
	if (node->refresh_frame_valid != 0) {
		advance = nvrtx_disp_frame_advance(node->refresh_frame, frame);
		if (advance != 0U) {
			node->refresh_count += advance;
			node->refresh_time_ns = kern_ticks_to_ns(sched_ticks());
		}
	}

	/* The next reading counts from this one. */
	node->refresh_frame = frame;
	node->refresh_frame_valid = 1;

	/* The count and its time, moved or not. */
	*count = node->refresh_count;
	*time_ns = node->refresh_time_ns;

	spin_unlock_irqrestore(&node->refresh_lock, irq);
}

/* Describes the one display (the display query operation). */
static int
present_query(
	void *private_data,
	void *session,
	struct gpu_display_info *request)
{
	struct nvrtx_node *node;

	UNUSED_PARAMETER(session);

	node = private_data;

	/* One display exists. */
	request->count = 1U;

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Only display 0 exists. */
	if (request->index != 0U)
		return EINVAL;

	/* The display, connected, FIFO, showing blobs, counting its refreshes. */
	request->display_id = NVRTX_NODE_DISPLAY_ID;
	request->generation = NVRTX_NODE_GENERATION;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB | GPU_DISPLAY_REFRESH_COUNTER;

	/* Active while a lease has frames on it; a cursor plane when its channel was made. */
	mutex_lock(&node->lock);

	if (node->owner != NULL && node->sequence != 0U)
		request->flags |= GPU_DISPLAY_ACTIVE;
	if (node->cursor_ready != 0)
		request->flags |= GPU_DISPLAY_HAS_CURSOR;

	mutex_unlock(&node->lock);

	/* One plane, the two packed formats, and the largest frame. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = PRESENT_MAX_FRAME_BYTES;

	/* The head's mode is the current, preferred and largest one. */
	request->current_width = node->width;
	request->current_height = node->height;
	request->preferred_width = node->width;
	request->preferred_height = node->height;
	request->max_width = node->width;
	request->max_height = node->height;
	request->refresh_millihz = node->refresh_millihz;

	/* The physical size needs the EDID, which is not read yet. */
	request->physical_width_mm = 0U;
	request->physical_height_mm = 0U;
	kern_memcpy(request->name, "nvrtx head", sizeof("nvrtx head"));

	/* Succeeded: the display is described. */
	return 0;
}

/* Enumerates or checks the one mode (the display mode operation): the head's. */
static int
present_mode(
	void *private_data,
	void *session,
	struct gpu_display_mode *request)
{
	struct nvrtx_node *node;

	UNUSED_PARAMETER(session);

	node = private_data;

	/* Only the one display of this generation exists. */
	if (request->display_id != NVRTX_NODE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != NVRTX_NODE_GENERATION)
		return ESTALE;

	/* Enumeration: the one mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;

		request->width = node->width;
		request->height = node->height;
		request->refresh_millihz = node->refresh_millihz;
		return 0;
	}

	/* Validation: only the head's mode, at its refresh (0 asks for it). */
	if (request->width != node->width || request->height != node->height)
		return EINVAL;
	if (request->refresh_millihz != 0U && request->refresh_millihz != node->refresh_millihz)
		return EINVAL;

	/* Succeeded: the mode is the display's, at its refresh. */
	request->refresh_millihz = node->refresh_millihz;
	return 0;
}

/* Gives the display's lease to a session (the display claim operation); EBUSY while another holds it. */
static int
present_claim(
	void *private_data,
	void *session,
	struct gpu_display_claim *request)
{
	struct nvrtx_node *node;

	node = private_data;

	/* Only plane 0 of the one display of this generation exists. */
	if (request->display_id != NVRTX_NODE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != NVRTX_NODE_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

	/* Takes the lease when it is free and the display works. */
	mutex_lock(&node->lock);

	if (node->broken != 0) {
		mutex_unlock(&node->lock);
		return EIO;
	}

	if (node->owner != NULL) {
		mutex_unlock(&node->lock);
		return EBUSY;
	}

	/* The session holds a new lease with no frame yet; the console surface stays until the first. */
	node->owner = session;
	node->lease = node->next_lease;
	node->next_lease++;
	node->sequence = 0U;
	request->lease = node->lease;

	mutex_unlock(&node->lock);

	/* Succeeded: the session holds the display. */
	nvrtx_stage_mark("gpu-device lease %llu claimed", (unsigned long long)request->lease);
	return 0;
}

/* Ends the lease of its holder (the display release operation); EINVAL for another session. */
static int
present_release(
	void *private_data,
	void *session,
	const struct gpu_display_release *request)
{
	struct nvrtx_node *node;

	node = private_data;

	/* Ends the lease, bringing the console surface back, after a presentation still running. */
	mutex_lock(&node->present_lock);
	mutex_lock(&node->lock);

	if (node->owner != session || node->lease != request->lease) {
		mutex_unlock(&node->lock);
		mutex_unlock(&node->present_lock);
		return EINVAL;
	}

	present_end_lease(node);

	mutex_unlock(&node->lock);
	mutex_unlock(&node->present_lock);

	/* Succeeded: the lease is free. */
	return 0;
}

/*
 * Shows a frame of the lease holder (the display present operation).
 *
 * A blob is flipped to as it is: its rows a multiple of 64 bytes apart, its
 * image on a 256-byte boundary of the VRAM.  Storage is copied into the next
 * of the node's frames, which is flipped to.  Either way the frame has the
 * display's size and the call returns once the flip has begun.
 *
 * The presentation holds the node's present lock throughout, so frames,
 * the lease's end and a blob's going off the screen happen one at a time;
 * it holds the node's lock only to check the lease and to send the flip,
 * not while the storage is copied or the flip waits for the head's next
 * refresh, so the renderer and the cursor go on meanwhile.  Returns 0 with
 * the frame's sequence, EINVAL for another session's lease or a frame the
 * display cannot show, ESTALE for another generation, or EIO when the flip
 * did not begin (the display is then broken).
 */
static int
present_present(
	void *private_data,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;
	struct nvrtx_disp_surface surface;
	uint64_t waited;
	uint32_t notifier;
	bool blob;
	int error;

	node = private_data;
	resource = object;

	/* A blob is flipped to as it is; storage is copied first. */
	blob = false;
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U)
		blob = true;

	/* The frame has the display's size. */
	if (request->width != node->width || request->height != node->height)
		return EINVAL;

	/* One presentation at a time; the lease cannot end while it runs. */
	mutex_lock(&node->present_lock);

	/* Checks the lease and picks the surface under the node's lock. */
	mutex_lock(&node->lock);

	error = present_admit(node, session, resource, request, blob, &surface);

	mutex_unlock(&node->lock);

	/* Reports a frame the lease or the display does not take. */
	if (error != 0) {
		mutex_unlock(&node->present_lock);
		return error;
	}

	/* Storage goes into the node's frame not on the screen, which only presentations touch. */
	if (!blob)
		present_copy(node, resource, request, &surface);

	/* Sends the flip under the node's lock: the core's view change and the cursor's core updates take turns. */
	mutex_lock(&node->lock);

	error = nvrtx_scanout_frame_send(node->gpu, &surface, &notifier);

	mutex_unlock(&node->lock);

	/* Waits for the head's refresh that begins the flip, outside the node's lock, and counts the wait. */
	if (error == 0) {
		waited = nvrtx_now_us();
		error = nvrtx_scanout_frame_wait(node->gpu, notifier);
		waited = nvrtx_now_us() - waited;
		nvrtx_node_count(node, &node->counts.flips, 1U);
		nvrtx_node_count(node, &node->counts.flip_us, waited);
	}

	/* Records the frame on the screen, or the broken display. */
	mutex_lock(&node->lock);

	if (error == 0) {
		present_shown(node, resource, blob);
		request->sequence = node->sequence;
	} else {
		present_broken(node, error);
		if (blob)
			node->shown = resource;
	}

	mutex_unlock(&node->lock);

	mutex_unlock(&node->present_lock);

	/* Reports a flip that did not begin. */
	if (error != 0)
		return EIO;

	/* Succeeded: the frame is on the screen. */
	return 0;
}

/*
 * Reports the display's topology sequence (the display events operation):
 * one, which nothing moves yet (the resource manager's hotplug events are
 * not taken).  Any context: no lock, no hardware access, no wait.
 */
static int
present_events(
	void *private_data,
	void *session,
	uint64_t *sequence)
{
	UNUSED_PARAMETER(private_data);
	UNUSED_PARAMETER(session);

	/* Succeeded: the first and only topology. */
	*sequence = NVRTX_NODE_TOPOLOGY_FIRST;
	return 0;
}

/*
 * Answers a power request for the display (the display power operation):
 * after the checks of the display and of the lease, EOPNOTSUPP, since the
 * output has no power control here (GPU_DISPLAY_POWER_CONTROL is not in its
 * flags).  ENOENT for another display, ESTALE for another generation, EBUSY
 * when this open does not hold the lease.
 */
static int
present_power(
	void *private_data,
	void *session,
	const struct gpu_display_power *request)
{
	struct nvrtx_node *node;
	void *owner;

	node = private_data;

	/* Only the one display of this generation exists. */
	if (request->display_id != NVRTX_NODE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != NVRTX_NODE_GENERATION)
		return ESTALE;

	/* Reads who holds the lease. */
	mutex_lock(&node->lock);

	owner = node->owner;

	mutex_unlock(&node->lock);

	/* Only the lease's holder may ask. */
	if (owner != session)
		return EBUSY;

	/* The output cannot be switched off and on without the resource manager's modeset. */
	return EOPNOTSUPP;
}

/*
 * Waits for the display's refresh boundary after the request's cursor, or
 * reports the count at once for cursor 0 (the display refresh operation).
 *
 * The boundaries are the head's frames: its frame counter is read each
 * scheduler tick while the caller waits (no interrupt is used).  Returns 0
 * with sequence and time_ns, ENOENT for another display, ESTALE for another
 * generation, or ETIMEDOUT.
 */
static int
present_refresh_wait(
	void *private_data,
	void *session,
	struct gpu_display_refresh *request)
{
	struct nvrtx_node *node;
	uint64_t deadline;
	uint64_t now;
	uint64_t count;
	uint64_t time_ns;

	UNUSED_PARAMETER(session);

	node = private_data;

	/* Only the one display of this generation exists. */
	if (request->display_id != NVRTX_NODE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != NVRTX_NODE_GENERATION)
		return ESTALE;

	/* The deadline in scheduler ticks, rounded up. */
	now = sched_ticks();
	deadline = now + kern_ns_to_ticks(request->timeout_ns);

	/* Until a boundary after the cursor, or the deadline. */
	for (;;) {
		/* The count now. */
		nvrtx_present_refresh_sample(node, &count, &time_ns);

		/* Cursor zero, or a boundary after the cursor: reported. */
		if (request->cursor == 0U || count > request->cursor) {
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
		sched_sleep(now + kern_ms_to_ticks(PRESENT_POLL_MS));
	}

	/* Succeeded: the boundary after the cursor. */
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
	struct nvrtx_node *node;

	node = private_data;

	/* Reads the lease's newest frame under the node's lock. */
	mutex_lock(&node->lock);

	if (node->owner != session || node->lease != request->lease) {
		mutex_unlock(&node->lock);
		return EBUSY;
	}

	if (request->sequence > node->sequence) {
		mutex_unlock(&node->lock);
		return EINVAL;
	}

	if (node->sequence == 0U) {
		mutex_unlock(&node->lock);
		return EAGAIN;
	}

	request->completed_sequence = node->sequence;
	request->present_time_ns = node->shown_time_ns;
	request->generation = NVRTX_NODE_GENERATION;

	mutex_unlock(&node->lock);

	/* Succeeded: the sequence has been shown. */
	return 0;
}

/*
 * Reports the head's refresh in millihertz from the pixel clock and raster of
 * the mode takeover set.
 */
static uint32_t
present_refresh(
	const struct nvrtx_display_state *display)
{
	uint64_t hertz;
	uint64_t pixels;
	uint64_t millihertz;
	uint32_t raster;
	uint32_t clock;

	/* The raster's whole width and height (blanking included), and the clock. */
	raster = display->mode.raster_size;
	clock = display->mode.clock_hz;
	pixels = (uint64_t)(raster & 0x7fffU) * ((raster >> 16) & 0x7fffU);
	hertz = clock & PRESENT_CLOCK_HERTZ_MASK;

	/* An unreadable raster is taken as 60 Hz. */
	if (pixels == 0U || hertz == 0U)
		return PRESENT_REFRESH_DEFAULT;

	/* The clock scaled by 1000/1001 when the head says so. */
	millihertz = hertz * 1000U / pixels;
	if ((clock & PRESENT_CLOCK_ADJUST) != 0U)
		millihertz = millihertz * 1000U / 1001U;

	/* Succeeded: the frames per thousand seconds. */
	return (uint32_t)millihertz;
}

/*
 * Checks a presentation against the lease and the display and describes the
 * surface it flips to: the blob itself, or the node's frame not on the
 * screen, which the storage is to be copied into.  The caller holds the
 * node's present lock and its lock.  Returns 0, EINVAL, ESTALE or EIO as
 * present_present() does.
 */
static int
present_admit(
	struct nvrtx_node *node,
	void *session,
	const struct nvrtx_resource *resource,
	const struct gpu_display_present *request,
	bool blob,
	struct nvrtx_disp_surface *surface)
{
	uint64_t offset;

	/* Only the lease's holder presents. */
	if (node->owner != session || node->lease != request->lease)
		return EINVAL;

	/* Only the one generation exists. */
	if (request->generation != NVRTX_NODE_GENERATION)
		return ESTALE;

	/* A broken display flips nothing more. */
	if (node->broken != 0)
		return EIO;

	/* A blob is the surface itself; storage goes into the next of the node's frames. */
	if (blob) {
		/* Only a VRAM blob has a surface (a blob only the CPU touches has no backing). */
		if (resource->kind != NVRTX_RESOURCE_BLOB)
			return EINVAL;
		offset = resource->backing->bo.vram + request->offset;
		if ((offset % PRESENT_OFFSET_ALIGN) != 0U ||
		    (request->stride % PRESENT_PITCH_ALIGN) != 0U)
			return EINVAL;

		surface->offset = offset;
		surface->pitch = request->stride;
	} else {
		if (resource->kind != NVRTX_RESOURCE_STORAGE || (request->offset % 4U) != 0U)
			return EINVAL;

		*surface = node->frames[node->next];
	}

	/* The frame's size and format. */
	surface->width = request->width;
	surface->height = request->height;
	surface->handle = NVRTX_DISP_HANDLE_VRAM;
	surface->format = present_format(request->format);

	/* Succeeded: the frame may be shown. */
	return 0;
}

/*
 * Records a frame whose flip has begun: the blob on the screen, or none
 * (the next storage frame then goes to the other of the node's frames,
 * since this one is on the screen), and the frame's number and time.  The
 * caller holds the node's lock.
 */
static void
present_shown(
	struct nvrtx_node *node,
	struct nvrtx_resource *resource,
	bool blob)
{
	/* The blob a later going off the screen looks for, or the other frame for the next copy. */
	node->shown = NULL;
	if (blob) {
		node->shown = resource;
	} else {
		node->next ^= 1U;
	}

	/* The frame counts: its number and the time it was shown. */
	node->sequence++;
	node->shown_time_ns = nvrtx_now_us() * 1000U;
}

/*
 * Flips the window to a surface and waits until the flip has begun; a flip
 * that does not begin breaks the display.  The caller holds the node's
 * present lock and its lock.
 */
static int
present_show(
	struct nvrtx_node *node,
	const struct nvrtx_disp_surface *surface)
{
	int error;

	/* Flips and waits until the flip has begun. */
	error = nvrtx_scanout_frame(node->gpu, surface);
	if (error != 0) {
		present_broken(node, error);
		return error;
	}

	/* Succeeded: the surface is on the screen. */
	return 0;
}

/*
 * Marks the display broken after a flip that did not begin: nothing is
 * flipped after it.  The caller holds the node's lock.
 */
static void
present_broken(
	struct nvrtx_node *node,
	int error)
{
	/* Presentations, the cursor and the lease's end leave the head alone from now on. */
	node->broken = 1;
	nvrtx_stage_mark("gpu-device fail: a flip did not begin (%d); the display stops", error);
}

/*
 * Ends the lease: the console surface comes back and the blob on the screen
 * is let go (kept when the flip back failed).  The caller holds the node's
 * present lock and its lock.
 */
static void
present_end_lease(
	struct nvrtx_node *node)
{
	int error;

	/* The lease's cursor goes, image and all. */
	nvrtx_cursor_lease_end(node);

	/* Flips back to the console surface when a frame of the lease is up. */
	if (node->sequence != 0U && node->broken == 0) {
		error = present_show(node, &node->gpu->display.surfaces[0]);
		if (error != 0 && node->shown != NULL)
			node->shown->backing->kept = 1;
		node->shown = NULL;
	}

	/* A broken display keeps the blob it may still read. */
	if (node->shown != NULL) {
		node->shown->backing->kept = 1;
		node->shown = NULL;
	}

	/* The lease is free. */
	nvrtx_stage_mark("gpu-device lease %llu released after %llu frames", (unsigned long long)node->lease, (unsigned long long)node->sequence);
	node->owner = NULL;
	node->lease = 0U;
	node->sequence = 0U;
}

/*
 * Copies a storage frame's rows into one of the node's frames through the
 * CPU's window.  The caller checked the frame has the display's size; the
 * core checked its rows lie inside the storage.
 */
static void
present_copy(
	struct nvrtx_node *node,
	const struct nvrtx_resource *resource,
	const struct gpu_display_present *request,
	const struct nvrtx_disp_surface *frame)
{
	volatile uint8_t *target_base;
	volatile uint32_t *target;
	const uint32_t *source;
	uint32_t row;
	uint32_t column;

	/* The frame the copy goes to is the one not on the screen. */
	target_base = nvrtx_bo_cpu(node->gpu, &node->frame_objects[node->next]);

	/* Copies each row word by word. */
	for (row = 0U; row < request->height; row++) {
		source = (const uint32_t *)(const void *)(resource->cpu + request->offset + (uint64_t)row * request->stride);
		target = (volatile uint32_t *)(volatile void *)(target_base + (uint64_t)row * frame->pitch);
		for (column = 0U; column < request->width; column++)
			target[column] = source[column];
	}

	/* Reads the last word back, so the writes have reached the VRAM before the flip. */
	if (request->height != 0U)
		(void)target_base[0];
}

/* Reports the window's pixel format of a presentation's format. */
static uint32_t
present_format(
	uint32_t format)
{
	/* R, G, B, A in memory. */
	if (format == GPU_PIXEL_RGBA8888)
		return NVRTX_DISP_FORMAT_A8B8G8R8;

	/* B, G, R, A in memory. */
	return NVRTX_DISP_FORMAT_A8R8G8B8;
}
