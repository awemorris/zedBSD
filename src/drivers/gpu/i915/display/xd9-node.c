/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The external displays of a display 9 node (see xd9-node.h).
 *
 * The registers are reached through the device's MMIO by the register
 * xd register window; the outputs' work is xd9-outputs.c's.  This file
 * keeps the leases, the two scanout buffers of each lit output, their
 * mapping in the presenting session's address space, the GPU copies, the
 * waits for the flips (the live surface read every tick, without the
 * interrupt), and the hotplug work (every second on its own queue).
 *
 * An output whose mode set fails (a DP link that does not train, a
 * transcoder that does not start), or whose flip does not latch, gives
 * the mode set step up (xd9-step.h): every external display is
 * darkened and withdrawn, as if its sink went, and the ports are not
 * looked at again, so the panel's display goes on alone.  The flips of the
 * panel failing withdraws them the same way, at the hotplug work's next
 * look.  Running out of pipes, DPLLs or buffer space is not a failure: the
 * claim is refused and the others go on.
 *
 * XXX: one device.
 */

#include "internal.h"
#include "display.h"
#include "xd9-step.h"
#include "xd9-flip.h"
#include "xd9-node.h"
#include "xd9-outputs.h"
#include "xd-clock.h"
#include "xd-hpd.h"
#include "xd-io.h"
#include "xd-platform.h"
#include "present.h"
#include "scanout.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../ggtt.h"
#include "../memory.h"
#include "../mmio.h"
#include "../power.h"
#include "../ppgtt.h"
#include "../session.h"
#include "../sync.h"
#include "../workqueue.h"
#include "../vk/vk-blit.h"
#include "../intel/display-regs-xd9.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The display id of port B; port C's and D's follow. */
#define I915_XD9_NODE_ID_FIRST		2U

/* How often the hotplug work reads the ports, in milliseconds, and how long a stop waits for it, in seconds. */
#define I915_XD9_NODE_POLL_MS		1000U
#define I915_XD9_NODE_STOP_S		3U

/* A lease of an external display: its port in the top byte, a count below. */
#define I915_XD9_NODE_LEASE_SHIFT	56U
#define I915_XD9_NODE_LEASE_MASK	((1ULL << I915_XD9_NODE_LEASE_SHIFT) - 1ULL)

/* The largest frame a session presents (as the panel's), and how long a flip may take to latch. */
#define I915_XD9_NODE_MAX_FRAME_BYTES	(64ULL << 20)
#define I915_XD9_NODE_LATCH_MS		100U

/* The bytes of a GGTT page. */
#define I915_XD9_NODE_PAGE_BYTES	4096U

/* The PCODE mailbox of the memory latencies (GEN9_PCODE_READ_MEM_LATENCY). */
#define I915_XD9_NODE_PCODE_LATENCY	0x6U

/*
 * One external display: its two buffers and their mapping, the flips, and
 * the lease.
 */
struct i915_xd9_screen {
	/* The two buffers (made at the claim), and where the presenting session's space maps them. */
	struct i915_scanout buffer[2];
	int made[2];
	struct i915_ppgtt *map_vm;
	uint64_t map_va[2];

	/* The buffer the next frame goes into, the surface last armed (0: none), and the tick it latched at. */
	unsigned back;
	uint32_t armed;
	uint64_t latched_tick;

	/* Darkened by a sleep, and the surface it showed, which the resume shows again. */
	int slept;
	uint32_t slept_surf;

	/* The lease: its holder (NULL: free), its number, the attachment it was given for, and its presentations. */
	void *owner;
	uint64_t lease;
	uint64_t generation;
	uint64_t sequence;
};

/*
 * The external displays of the node.
 *
 * The mutex covers everything but the events' sequence, which its own
 * spinlock covers so that any context can read it.
 */
struct i915_xd9_node {
	int started;
	struct i915_device *device;
	struct i915_xd_io io;
	struct mutex mutex;
	struct i915_xd9_outputs outputs;
	struct i915_xd9_screen screen[I915_XD9_OUTPUTS];
	uint64_t next_lease;

	/* The hotplug work, armed every second on its own queue, and whether a stop was asked. */
	struct i915_workqueue workqueue;
	struct i915_timer_queue timers;
	struct i915_delayed_work poll_work;
	int queues;
	volatile int stopping;

	/* Nonzero while the system sleeps: the ports are not looked at. */
	int sleeping;

	/*
	 * Nonzero once the mode set step was given up: every output is dark
	 * and shown as without a sink, and the ports are not looked at again.
	 * The hotplug work moves the events' sequence once for it and sets
	 * announced, so the display sessions hear of it within a second.  Both
	 * are set under the mutex and never cleared until the node stops.
	 */
	int withdrawn;
	int announced;

	/* The display events' sequence. */
	struct spinlock sequence_lock;
	uint64_t sequence;
};

static struct i915_xd9_node i915_xd9_node_state;

static void i915_xd9_node_latencies(struct i915_device *device, uint16_t latency[8]);
static void i915_xd9_node_work(void *argument);
static void i915_xd9_node_poll(struct i915_xd9_node *node);
static void i915_xd9_node_bump(struct i915_xd9_node *node);
static int i915_xd9_node_index(uint32_t display_id, unsigned *index);
static int i915_xd9_node_lease_index(struct i915_xd9_node *node, void *session, uint64_t lease, unsigned *index);
static int i915_xd9_node_make_buffers(struct i915_xd9_node *node, struct i915_xd9_screen *screen, const struct i915_display_mode *mode);
static void i915_xd9_node_free_buffers(struct i915_xd9_screen *screen);
static int i915_xd9_node_light(struct i915_xd9_node *node, unsigned index, uint32_t surf);
static void i915_xd9_node_darken(struct i915_xd9_node *node, unsigned index);
static void i915_xd9_node_end(struct i915_xd9_node *node, unsigned index);
static int i915_xd9_node_map(struct i915_xd9_node *node, struct i915_xd9_screen *screen, struct i915_ppgtt *vm);
static void i915_xd9_node_unmap(struct i915_xd9_node *node, struct i915_xd9_screen *screen);
static int i915_xd9_node_latch(struct i915_xd9_node *node, unsigned index);
static void i915_xd9_node_failed(struct i915_xd9_node *node, const char *reason, int error);
static void i915_xd9_node_withdraw(struct i915_xd9_node *node);

/*
 * Starts the external displays: the panel must be flipped with its timing
 * read; the CD clock, the memory latencies and the part come from the
 * registers and PCODE; the first look at the ports is taken at once, then
 * the hotplug work takes over.
 */
void
drv_i915_xd9_node_start(
	struct i915_device *device)
{
	struct i915_xd9_node *node;
	struct i915_xd9_panel panel;
	struct i915_mmio *mmio;
	uint16_t latency[8];
	uint32_t cdclk;
	uint32_t lcpll1;
	uint32_t ctrl1;
	uint32_t cdclk_ctl;
	char ports[I915_XD9_OUTPUTS + 1U];
	unsigned count;
	unsigned port;
	struct i915_xd_choice choice;
	int level;
	int error;

	/* The node, of the one device. */
	node = &i915_xd9_node_state;
	mmio = &device->gt.mmio;

	/* Only when the node's step (full, lowered by a step that failed) takes the mode set. */
	level = drv_i915_xd9_step();
	if (level < I915_XD9_STEP_MODESET)
		return;

	/* The flipped panel the outputs share the display with; its watermarks need its pixel clock. */
	error = drv_i915_xd9_flip_panel(&panel);
	if (error != 0) {
		drv_i915_xd9_step_fail(I915_XD9_STEP_MODESET, "the panel is not flipped or its pixel clock was not read", 0);
		return;
	}

	/* The CD clock, which stays as the firmware set it. */
	lcpll1 = drv_i915_raw_read32(mmio, I915_XD9_LCPLL1_CTL);
	ctrl1 = drv_i915_raw_read32(mmio, I915_XD9_DPLL_CTRL1);
	cdclk_ctl = drv_i915_raw_read32(mmio, I915_XD9_CDCLK_CTL);
	cdclk = drv_i915_xd9_cdclk_khz(lcpll1, ctrl1, cdclk_ctl);
	if (cdclk == 0U) {
		drv_i915_xd9_step_fail(I915_XD9_STEP_MODESET, "the CD clock could not be read", 0);
		return;
	}

	/* The node: its register table, its lock, its outputs. */
	kern_memset(node, 0, sizeof(*node));
	node->device = device;
	drv_i915_xd_io_init(&node->io, mmio, NULL);
	(void)mutex_init(&node->mutex, LOCK_RANK_DEVICE, "i915 xd9 node");
	spin_init(&node->sequence_lock, LOCK_RANK_DEVICE, "i915 xd9 node events");
	node->sequence = 1U;
	node->next_lease = 1U;
	error = drv_i915_xd_platform_choose_display9(device->product, 0U, 0, 0U, &choice);
	if (error != 0) {
		drv_i915_xd9_step_fail(I915_XD9_STEP_MODESET, "the part has no display 9 description", error);
		return;
	}
	i915_xd9_node_latencies(device, latency);
	drv_i915_xd9_outputs_init(&node->outputs, &node->io, choice.platform, &panel, cdclk, latency);

	/* The first look at the ports. */
	mutex_lock(&node->mutex);

	(void)drv_i915_xd9_outputs_poll(&node->outputs);

	mutex_unlock(&node->mutex);

	/* The hotplug work, every second on its own queue. */
	node->started = 1;
	error = drv_i915_workqueue_create(&node->workqueue, "i915-xd9-node");
	if (error == 0) {
		error = drv_i915_timer_queue_create(&node->timers, &node->workqueue, "i915-xd9-node-timer");
		if (error != 0)
			drv_i915_workqueue_destroy(&node->workqueue);
	}

	/* The first look a second from now. */
	if (error == 0) {
		node->queues = 1;
		drv_i915_delayed_work_init(&node->poll_work, i915_xd9_node_work, node);
		(void)drv_i915_delayed_queue(&node->timers, &node->poll_work, I915_XD9_NODE_POLL_MS);
	} else {
		kern_logf("i915: xd9-node: the hotplug work could not start: %d; the ports are not watched\n", error);
	}

	/* The ports' letters for the log. */
	count = 0U;
	for (port = I915_XD9_HPD_FIRST_DDI; port <= I915_XD9_HPD_LAST_DDI; port++) {
		if ((node->outputs.present & (1U << port)) != 0U)
			ports[count++] = (char)('A' + (int)port);
	}

	/* Succeeded: the external displays are offered. */
	ports[count] = '\0';
	kern_logf("i915: xd9-node: external displays on ports %s (connected 0x%x), panel on pipe %c, CD clock %u kHz, latencies %u/%u/%u/%u/%u/%u/%u/%u us\n",
	    ports,
	    node->outputs.connected,
	    'A' + (int)panel.pipe,
	    cdclk,
	    latency[0], latency[1], latency[2], latency[3], latency[4], latency[5], latency[6], latency[7]);
}

/* Darkens every external display and stops the hotplug work. */
void
drv_i915_xd9_node_stop(
	struct i915_device *device)
{
	struct i915_xd9_node *node;
	uint64_t deadline;
	unsigned index;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* Nothing started. */
	if (!node->started)
		return;

	/* The hotplug work cancelled and waited for, then its queues. */
	node->stopping = 1;
	if (node->queues) {
		deadline = sched_ticks() + kern_ms_to_ticks((uint64_t)I915_XD9_NODE_STOP_S * 1000U);
		(void)drv_i915_delayed_cancel_sync(&node->timers, &node->poll_work, deadline);
		drv_i915_timer_queue_destroy(&node->timers);
		drv_i915_workqueue_destroy(&node->workqueue);
		node->queues = 0;
	}

	/* Every lit output dark, every buffer given back. */
	mutex_lock(&node->mutex);

	for (index = 0; index < I915_XD9_OUTPUTS; index++)
		i915_xd9_node_end(node, index);

	mutex_unlock(&node->mutex);

	/* Succeeded: no external display. */
	node->started = 0;
}

/* Reports how many external displays have a sink now. */
uint32_t
drv_i915_xd9_node_count(void)
{
	struct i915_xd9_node *node;
	uint32_t count;
	unsigned index;

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* Nothing started. */
	if (!node->started)
		return 0U;

	/* Each output with a sink. */
	count = 0U;
	for (index = 0; index < I915_XD9_OUTPUTS; index++) {
		if (node->outputs.output[index].connected)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Reports whether a display id is an external display's. */
int
drv_i915_xd9_node_owns_display(
	uint32_t display_id)
{
	unsigned index;
	int error;

	/* Nothing started. */
	if (!i915_xd9_node_state.started)
		return 0;

	/* Succeeded: one of ports B to D. */
	error = i915_xd9_node_index(display_id, &index);
	return error == 0;
}

/* Reports whether a lease is an external display's (its top byte names a port). */
int
drv_i915_xd9_node_owns_lease(
	uint64_t lease)
{
	uint64_t port;

	/* Nothing started. */
	if (!i915_xd9_node_state.started)
		return 0;

	/* Succeeded: a port B to D in the top byte. */
	port = lease >> I915_XD9_NODE_LEASE_SHIFT;
	return port >= I915_XD9_HPD_FIRST_DDI && port <= I915_XD9_HPD_LAST_DDI;
}

/* Reports the display events' sequence, or 0 when the node is not started. */
uint64_t
drv_i915_xd9_node_sequence(void)
{
	struct i915_xd9_node *node;
	unsigned long enabled;
	uint64_t sequence;

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* Nothing started. */
	if (!node->started)
		return 0U;

	/* The sequence under its lock. */
	enabled = spin_lock_irqsave(&node->sequence_lock);

	sequence = node->sequence;

	spin_unlock_irqrestore(&node->sequence_lock, enabled);

	/* Succeeded: the sequence. */
	return sequence;
}

/*
 * Describes the index-th external display with a sink: connected, FIFO
 * presentation of shared frames, one plane, the mode chosen from its EDID;
 * LIMITED when no pipe is left to light it.  Returns 0, or EINVAL for an
 * index past them.
 */
int
drv_i915_xd9_node_query(
	void *device,
	uint32_t index,
	struct gpu_display_info *request)
{
	struct i915_xd9_node *node;
	struct i915_xd9_ext *output;
	struct i915_xd9_screen *screen;
	unsigned lit;
	unsigned found;
	unsigned slot;
	int error;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* The output under the node's lock. */
	mutex_lock(&node->mutex);

	/* The index-th output with a sink, and how many are lit. */
	output = NULL;
	screen = NULL;
	lit = 0U;
	found = 0U;
	for (slot = 0; slot < I915_XD9_OUTPUTS; slot++) {
		if (node->outputs.output[slot].lit)
			lit++;
		if (!node->outputs.output[slot].connected)
			continue;
		if (found == index && output == NULL) {
			output = &node->outputs.output[slot];
			screen = &node->screen[slot];
		}

		/* Counted among those with a sink. */
		found++;
	}

	/* An index past them. */
	error = EINVAL;
	if (output != NULL) {
		/* The display and its state. */
		request->display_id = I915_XD9_NODE_ID_FIRST + output->ddi - I915_XD9_HPD_FIRST_DDI;
		request->generation = output->generation;
		request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;
		if (screen->owner != NULL && screen->sequence != 0U)
			request->flags |= GPU_DISPLAY_ACTIVE;
		if (!output->lit && lit + 1U >= I915_XD9_PIPES)
			request->flags |= GPU_DISPLAY_LIMITED;

		/* One plane, the two packed formats, the one mode. */
		request->plane_count = 1U;
		request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
		request->max_frame_bytes = I915_XD9_NODE_MAX_FRAME_BYTES;
		request->current_width = output->mode.hdisplay;
		request->current_height = output->mode.vdisplay;
		request->preferred_width = output->mode.hdisplay;
		request->preferred_height = output->mode.vdisplay;
		request->max_width = output->mode.hdisplay;
		request->max_height = output->mode.vdisplay;
		request->refresh_millihz = drv_i915_xd9_ext_refresh(output);
		request->physical_width_mm = output->mode.width_mm;
		request->physical_height_mm = output->mode.height_mm;

		/* Its name: the kind and the port. */
		kern_memcpy(request->name, "i915 HDMI-B", sizeof("i915 HDMI-B"));
		if (output->kind == I915_XD9_OUTPUT_DP)
			kern_memcpy(request->name, "i915 DP-B", sizeof("i915 DP-B"));
		request->name[kern_strlen(request->name) - 1U] = (char)('A' + (int)output->ddi);
		error = 0;
	}

	mutex_unlock(&node->mutex);

	/* Reports an index past them. */
	if (error != 0)
		return error;

	/* Succeeded: the display is described. */
	return 0;
}

/*
 * Enumerates or checks a mode of an external display: the one mode chosen
 * from its EDID, and any smaller extent at its refresh.
 */
int
drv_i915_xd9_node_mode(
	void *device,
	void *session,
	struct gpu_display_mode *request)
{
	struct i915_xd9_node *node;
	struct i915_xd9_ext *output;
	uint32_t refresh;
	unsigned index;
	int error;

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(session);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* Only an external display's id. */
	error = i915_xd9_node_index(request->display_id, &index);
	if (error != 0)
		return ENOENT;

	/* The output under the node's lock. */
	mutex_lock(&node->mutex);

	output = &node->outputs.output[index];
	refresh = drv_i915_xd9_ext_refresh(output);
	error = 0;
	if (!output->connected) {
		/* A sink that is not there. */
		error = ENOENT;
	} else if (request->generation != output->generation) {
		/* Another attachment's. */
		error = ESTALE;
	} else if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		/* The one mode. */
		request->count = 1U;
		if (request->index != GPU_DISPLAY_COUNT_ONLY && request->index != 0U)
			error = EINVAL;
		if (request->index == 0U) {
			request->width = output->mode.hdisplay;
			request->height = output->mode.vdisplay;
			request->refresh_millihz = refresh;
		}
	} else if (request->width == 0U || request->height == 0U || request->width > output->mode.hdisplay || request->height > output->mode.vdisplay) {
		/* An extent that does not fit. */
		error = EINVAL;
	} else if (request->refresh_millihz != 0U && request->refresh_millihz != refresh) {
		/* Another refresh. */
		error = EINVAL;
	} else {
		/* Shown unscaled at the mode's refresh. */
		request->refresh_millihz = refresh;
	}

	mutex_unlock(&node->mutex);

	/* Reports a mode that is not taken. */
	if (error != 0)
		return error;

	/* Succeeded: the mode is shown. */
	return 0;
}

/*
 * Gives an external display's lease to a session and lights the output
 * showing black.  Returns 0 with the lease, ENOENT or ESTALE for another
 * display, EINVAL for a plane other than 0, EBUSY while another session
 * holds it, ENOSPC when no pipe is left, or the buffers' or the mode
 * set's error.
 */
int
drv_i915_xd9_node_claim(
	void *device,
	void *session,
	struct gpu_display_claim *request)
{
	struct i915_xd9_node *node;
	struct i915_xd9_ext *output;
	struct i915_xd9_screen *screen;
	unsigned index;
	int light_tried;
	int error;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* Only plane 0 of an external display. */
	error = i915_xd9_node_index(request->display_id, &index);
	if (error != 0)
		return ENOENT;
	if (request->plane_index != 0U)
		return EINVAL;

	/* The output under the node's lock. */
	mutex_lock(&node->mutex);

	output = &node->outputs.output[index];
	screen = &node->screen[index];
	error = 0;
	if (!output->connected)
		error = ENOENT;
	else if (request->generation != output->generation)
		error = ESTALE;
	else if (screen->owner != NULL)
		error = EBUSY;

	/* The buffers, the output lit, the lease. */
	if (error == 0)
		error = i915_xd9_node_make_buffers(node, screen, &output->mode);
	light_tried = 0;
	if (error == 0) {
		error = i915_xd9_node_light(node, index, (uint32_t)screen->buffer[0].surf);
		light_tried = 1;
	}

	/* A lit output becomes the session's screen with a new lease. */
	if (error == 0) {
		screen->owner = session;
		screen->lease = ((uint64_t)output->ddi << I915_XD9_NODE_LEASE_SHIFT) | (node->next_lease & I915_XD9_NODE_LEASE_MASK);
		node->next_lease++;
		screen->generation = output->generation;
		screen->sequence = 0U;
		request->lease = screen->lease;
	} else {
		i915_xd9_node_free_buffers(screen);
	}

	/* A mode set that failed (not a pipe, DPLL or buffer space that ran out) gives the external displays up. */
	if (light_tried &&
	    error != 0 &&
	    error != ENOSPC)
		i915_xd9_node_failed(node, "an external display could not be lit", error);

	mutex_unlock(&node->mutex);

	/* Reports a claim that was refused or failed. */
	if (error != 0)
		return error;

	/* Succeeded: the session holds the display. */
	kern_logf("i915: xd9-node: lease %llu of port %c claimed\n", (unsigned long long)request->lease, 'A' + (int)output->ddi);
	return 0;
}

/* Ends an external display's lease: the output dark, the buffers given back.  0, or EINVAL for another's lease. */
int
drv_i915_xd9_node_release(
	void *device,
	void *session,
	const struct gpu_display_release *request)
{
	struct i915_xd9_node *node;
	unsigned index;
	int error;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* The lease under the node's lock. */
	mutex_lock(&node->mutex);

	error = i915_xd9_node_lease_index(node, session, request->lease, &index);
	if (error == 0)
		i915_xd9_node_end(node, index);

	mutex_unlock(&node->mutex);

	/* Reports another's lease. */
	if (error != 0)
		return error;

	/* Succeeded: the lease is free. */
	return 0;
}

/*
 * Shows a frame on an external display: the flip armed before latches,
 * the GPU draws the frame into the buffer the plane does not show, and the
 * plane is pointed at it.  Returns 0 with the presented sequence, EINVAL
 * for another's lease or a frame the GPU cannot address, ESTALE for
 * another attachment, ENXIO when the sink went, ETIMEDOUT when the flip
 * before did not latch, or the mapping's or the copy's error.
 */
int
drv_i915_xd9_node_present(
	void *device,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct i915_xd9_node *node;
	struct i915_xd9_screen *screen;
	struct i915_xd9_ext *output;
	struct i915_session *owner;
	struct i915_gem_object *storage;
	struct i915_scanout *back;
	struct i915_gfx_surface src;
	struct i915_gfx_surface dst;
	struct i915_gfx_rect rect;
	unsigned index;
	int error;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;
	owner = session;
	storage = object;

	/* The lease and the frame under the node's lock. */
	mutex_lock(&node->mutex);

	error = i915_xd9_node_lease_index(node, session, request->lease, &index);
	screen = &node->screen[index];
	output = &node->outputs.output[index];
	if (error == 0 && request->generation != screen->generation)
		error = ESTALE;
	if (error == 0 && !output->lit)
		error = ENXIO;
	if (error == 0 && (owner->vk == NULL || storage->va == 0U || request->width == 0U || request->height == 0U))
		error = EINVAL;

	/* The buffers in the session's space, and the flip before latched. */
	if (error == 0)
		error = i915_xd9_node_map(node, screen, owner->vm);
	if (error == 0 && screen->armed != 0U) {
		error = i915_xd9_node_latch(node, index);
		if (error != 0)
			i915_xd9_node_failed(node, "an external display's flip did not latch", error);
	}

	/* The frame copied into the buffer the plane does not show, then the flip. */
	if (error == 0) {
		src.va = storage->va + request->offset;
		src.width = request->width;
		src.height = request->height;
		src.pitch = request->stride;
		src.format = VK_FORMAT_R8G8B8A8_UNORM;
		if (request->format == GPU_PIXEL_BGRA8888)
			src.format = VK_FORMAT_B8G8R8A8_UNORM;
		src.tiled = 0U;
		back = &screen->buffer[screen->back];
		dst.va = screen->map_va[screen->back];
		dst.width = back->width;
		dst.height = back->height;
		dst.pitch = back->pitch;
		dst.format = VK_FORMAT_B8G8R8A8_UNORM;
		dst.tiled = 0U;
		rect.x = 0;
		rect.y = 0;
		rect.w = request->width;
		if (rect.w > back->width)
			rect.w = back->width;
		rect.h = request->height;
		if (rect.h > back->height)
			rect.h = back->height;
		error = drv_i915_gfx_rect(owner->vk, &dst, &rect, &src, &rect, NULL, 0);
	}

	/* The plane pointed at the buffer; the next presentation or a wait waits for its latch. */
	if (error == 0) {
		drv_i915_xd9_outputs_flip(&node->outputs, index, (uint32_t)back->surf);
		screen->armed = (uint32_t)back->surf;
		screen->back ^= 1U;
		screen->sequence++;
		request->sequence = screen->sequence;
	}

	mutex_unlock(&node->mutex);

	/* Reports a frame that could not be shown. */
	if (error != 0)
		return error;

	/* Succeeded: the frame's flip is armed. */
	return 0;
}

/*
 * Waits until an external display's newest flip has latched.  Returns 0
 * with the completed sequence, EINVAL for another's lease or a sequence
 * not presented, EAGAIN before the first presentation, or ETIMEDOUT.
 */
int
drv_i915_xd9_node_wait(
	void *device,
	void *session,
	struct gpu_display_wait *request)
{
	struct i915_xd9_node *node;
	struct i915_xd9_screen *screen;
	unsigned index;
	int error;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* The lease under the node's lock; a flip that does not latch gives the external displays up. */
	mutex_lock(&node->mutex);

	error = i915_xd9_node_lease_index(node, session, request->lease, &index);
	screen = &node->screen[index];
	if (error == 0 && request->sequence > screen->sequence)
		error = EINVAL;
	if (error == 0 && screen->sequence == 0U)
		error = EAGAIN;
	if (error == 0 && screen->armed != 0U) {
		error = i915_xd9_node_latch(node, index);
		if (error != 0)
			i915_xd9_node_failed(node, "an external display's flip did not latch", error);
	}

	/* A latched flip completes the request at the screen's sequence. */
	if (error == 0) {
		request->completed_sequence = screen->sequence;
		request->present_time_ns = kern_ticks_to_ns(screen->latched_tick);
		request->generation = screen->generation;
	}

	mutex_unlock(&node->mutex);

	/* Reports a wait that could not be answered. */
	if (error != 0)
		return error;

	/* Succeeded: the sequence has completed. */
	return 0;
}

/* Ends the leases a closing session still holds. */
void
drv_i915_xd9_node_session_close(
	struct i915_device *device,
	void *session)
{
	struct i915_xd9_node *node;
	unsigned index;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;

	/* Nothing started. */
	if (!node->started)
		return;

	/* Each lease the session holds. */
	mutex_lock(&node->mutex);

	for (index = 0; index < I915_XD9_OUTPUTS; index++) {
		if (node->screen[index].owner == session) {
			kern_logf("i915: xd9-node: the holder of port %c's lease closed without releasing it\n", 'A' + (int)node->outputs.output[index].ddi);
			i915_xd9_node_end(node, index);
		}
	}

	mutex_unlock(&node->mutex);
}

/*
 * Darkens the lit external displays for a sleep, keeping their leases and
 * buffers: each remembers the surface it showed (the flip armed last, else
 * the live one).
 */
void
drv_i915_xd9_node_suspend(
	struct i915_device *device)
{
	struct i915_xd9_node *node;
	struct i915_xd9_screen *screen;
	unsigned index;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;
	if (!node->started)
		return;

	/* Each lit output, under the node's lock; the ports are not looked at until the resume. */
	mutex_lock(&node->mutex);

	node->sleeping = 1;
	for (index = 0; index < I915_XD9_OUTPUTS; index++) {
		screen = &node->screen[index];
		screen->slept = 0;
		if (!node->outputs.output[index].lit)
			continue;
		screen->slept_surf = screen->armed;
		if (screen->slept_surf == 0U)
			screen->slept_surf = drv_i915_xd9_outputs_live(&node->outputs, index);
		screen->slept = 1;
		screen->armed = 0U;
		i915_xd9_node_darken(node, index);
	}

	mutex_unlock(&node->mutex);
}

/*
 * Lights the external displays a sleep darkened again, on the surface
 * each showed; one that cannot be lit stays dark (its presentations
 * answer ENXIO) and the log says why.
 */
void
drv_i915_xd9_node_resume(
	struct i915_device *device)
{
	struct i915_xd9_node *node;
	struct i915_xd9_screen *screen;
	unsigned index;
	int failure;
	int error;

	UNUSED_PARAMETER(device);

	/* The node, of the one device. */
	node = &i915_xd9_node_state;
	if (!node->started)
		return;

	/* Each output the sleep darkened, under the node's lock; then the ports are looked at again. */
	mutex_lock(&node->mutex);

	failure = 0;
	for (index = 0; index < I915_XD9_OUTPUTS; index++) {
		screen = &node->screen[index];
		if (!screen->slept)
			continue;
		screen->slept = 0;
		error = i915_xd9_node_light(node, index, screen->slept_surf);
		if (error != 0) {
			kern_logf("i915: xd9-node: port %c could not be lit again after the sleep: %d\n", 'A' + (int)node->outputs.output[index].ddi, error);
			if (error != ENOSPC)
				failure = error;
		}
	}

	/* A mode set that failed after the sleep gives the external displays up. */
	if (failure != 0)
		i915_xd9_node_failed(node, "an external display could not be lit again after the sleep", failure);

	/* The ports are looked at again. */
	node->sleeping = 0;

	mutex_unlock(&node->mutex);
}

/*
 * Reads the memory latencies from PCODE (levels 0 to 3 with data 0, 4 to
 * 7 with data 1) and IPC's state, and decodes them.  A mailbox that does
 * not answer leaves conservative latencies (the latencies the host tests
 * use, which keep every level).
 */
static void
i915_xd9_node_latencies(
	struct i915_device *device,
	uint16_t latency[8])
{
	static const uint16_t fallback[8] = { 2U, 4U, 10U, 18U, 28U, 40U, 60U, 90U };
	struct i915_gt *gt;
	uint32_t low;
	uint32_t high;
	uint32_t extra;
	uint32_t arbiter;
	int ipc;
	int error;

	/* The GT: its PCODE mailbox and registers. */
	gt = &device->gt;

	/* The two words. */
	low = 0U;
	extra = 0U;
	error = drv_i915_pcode_read(&gt->sb_lock, &gt->mmio, I915_XD9_NODE_PCODE_LATENCY, &low, &extra);
	high = 1U;
	extra = 0U;
	if (error == 0)
		error = drv_i915_pcode_read(&gt->sb_lock, &gt->mmio, I915_XD9_NODE_PCODE_LATENCY, &high, &extra);
	if (error != 0) {
		kern_logf("i915: xd9-node: PCODE did not give the memory latencies (%d); conservative ones are used\n", error);
		kern_memcpy(latency, fallback, sizeof(fallback));
		return;
	}

	/* IPC on Kaby Lake and later. */
	arbiter = drv_i915_raw_read32(&gt->mmio, I915_XD9_DISP_ARB_CTL2);
	ipc = gt->info.platform != I915_PLATFORM_SKYLAKE && (arbiter & I915_XD9_IPC_ENABLE) != 0U;

	/* Succeeded: the latencies. */
	drv_i915_xd9_wm_latency(low, high, ipc, latency);
}

/* The hotplug work: a look at the ports, armed again a second later until the node stops. */
static void
i915_xd9_node_work(
	void *argument)
{
	struct i915_xd9_node *node;

	/* The node the work was armed for. */
	node = argument;

	/* A stop asked: no more looks. */
	if (node->stopping)
		return;

	/* Succeeded: a look, and the next one armed. */
	i915_xd9_node_poll(node);
	(void)drv_i915_delayed_queue(&node->timers, &node->poll_work, I915_XD9_NODE_POLL_MS);
}

/*
 * Looks at the ports: a lit output whose sink went is darkened and its
 * lease ended; any change moves the display events' sequence.
 */
static void
i915_xd9_node_poll(
	struct i915_xd9_node *node)
{
	unsigned changed;
	unsigned index;
	int announce;
	int step;

	/* The ports under the node's lock; none while the system sleeps. */
	mutex_lock(&node->mutex);

	if (node->sleeping) {
		mutex_unlock(&node->mutex);
		return;
	}

	/* A step below the mode set (the panel's flips failed, say) withdraws the external displays. */
	step = drv_i915_xd9_step();
	if (step < I915_XD9_STEP_MODESET)
		i915_xd9_node_withdraw(node);

	/* Withdrawn: the ports are not looked at again, and the sessions hear of it once. */
	if (node->withdrawn) {
		announce = 0;
		if (!node->announced)
			announce = 1;
		node->announced = 1;
		mutex_unlock(&node->mutex);
		if (announce)
			i915_xd9_node_bump(node);
		return;
	}

	/* Which ports changed; a lit output whose sink went is darkened and its lease ended. */
	changed = drv_i915_xd9_outputs_poll(&node->outputs);
	for (index = 0; index < I915_XD9_OUTPUTS; index++) {
		if (node->outputs.output[index].connected)
			continue;
		if (node->screen[index].owner == NULL && !node->outputs.output[index].lit)
			continue;
		kern_logf("i915: xd9-node: port %c's sink went; its display is dark and its lease ended\n", 'A' + (int)node->outputs.output[index].ddi);
		i915_xd9_node_end(node, index);
	}

	mutex_unlock(&node->mutex);

	/* A change is heard by the display sessions. */
	if (changed != 0U)
		i915_xd9_node_bump(node);
}

/* Moves the display events' sequence and wakes those waiting for it. */
static void
i915_xd9_node_bump(
	struct i915_xd9_node *node)
{
	unsigned long enabled;

	/* The sequence under its lock. */
	enabled = spin_lock_irqsave(&node->sequence_lock);

	node->sequence++;

	spin_unlock_irqrestore(&node->sequence_lock, enabled);

	/* Succeeded: the sessions hear of it. */
	poll_notify();
}

/* Finds the output of an external display id: 0, or ENOENT. */
static int
i915_xd9_node_index(
	uint32_t display_id,
	unsigned *index)
{
	/* Ports B to D, ids 2 to 4. */
	if (display_id < I915_XD9_NODE_ID_FIRST || display_id >= I915_XD9_NODE_ID_FIRST + I915_XD9_OUTPUTS)
		return ENOENT;

	/* Succeeded: the output. */
	*index = display_id - I915_XD9_NODE_ID_FIRST;
	return 0;
}

/*
 * Finds the output of a lease its session holds: 0 with its index (also
 * on failure, a valid index for the caller's convenience), or EINVAL.
 */
static int
i915_xd9_node_lease_index(
	struct i915_xd9_node *node,
	void *session,
	uint64_t lease,
	unsigned *index)
{
	uint64_t port;

	/* The port the lease names. */
	*index = 0;
	port = lease >> I915_XD9_NODE_LEASE_SHIFT;
	if (port < I915_XD9_HPD_FIRST_DDI || port > I915_XD9_HPD_LAST_DDI)
		return EINVAL;
	*index = (unsigned)port - I915_XD9_HPD_FIRST_DDI;

	/* Held by this session. */
	if (node->screen[*index].owner != session || node->screen[*index].lease != lease)
		return EINVAL;

	/* Succeeded: the lease is the session's. */
	return 0;
}

/*
 * Makes and pins the two buffers of a mode's extent, cleared to black and
 * made visible to the display engine.  Returns 0, or the scanout's error
 * (what was made is freed by the caller).
 */
static int
i915_xd9_node_make_buffers(
	struct i915_xd9_node *node,
	struct i915_xd9_screen *screen,
	const struct i915_display_mode *mode)
{
	unsigned index;
	int error;

	/* Each buffer: its backing, its GGTT range, black. */
	for (index = 0; index < 2U; index++) {
		error = drv_i915_scanout_create(&node->device->gt.mem, mode->hdisplay, mode->vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &screen->buffer[index]);
		if (error != 0)
			return error;
		screen->made[index] = 1;
		error = drv_i915_scanout_pin(&screen->buffer[index], "xd9 node");
		if (error != 0)
			return error;
		kern_memset(screen->buffer[index].cpu, 0, screen->buffer[index].size);
		drv_i915_scanout_publish(&screen->buffer[index]);
	}

	/* Succeeded: buffer 0 is shown first, the first frame goes into buffer 1. */
	screen->back = 1U;
	screen->armed = 0U;
	return 0;
}

/* Unpins and destroys the buffers made. */
static void
i915_xd9_node_free_buffers(
	struct i915_xd9_screen *screen)
{
	unsigned index;
	int error;

	/* Each buffer made: the GGTT range, then the backing. */
	for (index = 0; index < 2U; index++) {
		if (!screen->made[index])
			continue;
		if (screen->buffer[index].state >= I915_SCANOUT_PINNED) {
			error = drv_i915_scanout_unpin(&screen->buffer[index]);
			if (error != 0) {
				kern_logf("i915: xd9-node: XXX a buffer could not be unpinned: %d; it is kept\n", error);
				screen->made[index] = 0;
				continue;
			}
		}

		/* The backing. */
		error = drv_i915_scanout_destroy(&screen->buffer[index]);
		if (error != 0)
			kern_logf("i915: xd9-node: XXX a buffer could not be destroyed: %d\n", error);
		screen->made[index] = 0;
	}
}

/*
 * Lights an output on one of its buffers, with the panel's lease mutex
 * held while the panel's watermarks move (its flips write the same
 * surface register).  Returns 0, or the outputs' error.
 */
static int
i915_xd9_node_light(
	struct i915_xd9_node *node,
	unsigned index,
	uint32_t surf)
{
	struct i915_resident_display *rd;
	struct i915_scanout *first;
	int error;

	/* The buffers' stride, and the panel's lease, whose mutex holds its flips off. */
	first = &node->screen[index].buffer[0];
	rd = &node->device->display->rd;
	drv_i915_present_lease_init(node->device->display);

	/* The output lit, the panel's flips held off meanwhile. */
	mutex_lock(&rd->mutex);

	error = drv_i915_xd9_outputs_light(&node->outputs, index, surf, first->stride_units, 0);

	mutex_unlock(&rd->mutex);

	/* Reports an output that could not be lit. */
	if (error != 0)
		return error;

	/* Succeeded: the output shows black. */
	return 0;
}

/*
 * Ends an external display's use: the output dark (the panel's lease
 * mutex held while the watermarks move), the buffers unmapped and given
 * back, the lease free.
 */
static void
i915_xd9_node_end(
	struct i915_xd9_node *node,
	unsigned index)
{
	struct i915_xd9_screen *screen;

	/* The display the index names. */
	screen = &node->screen[index];

	/* The output dark. */
	i915_xd9_node_darken(node, index);

	/* The buffers no longer read, unmapped and freed; the lease free. */
	i915_xd9_node_unmap(node, screen);
	i915_xd9_node_free_buffers(screen);
	screen->owner = NULL;
	screen->lease = 0U;
	screen->armed = 0U;
	screen->sequence = 0U;
	screen->slept = 0;
}

/* Darkens an output, with the panel's lease mutex held while the watermarks move. */
static void
i915_xd9_node_darken(
	struct i915_xd9_node *node,
	unsigned index)
{
	struct i915_resident_display *rd;

	/* The panel's lease. */
	rd = &node->device->display->rd;
	drv_i915_present_lease_init(node->device->display);

	/* The output dark, the panel's flips held off meanwhile. */
	mutex_lock(&rd->mutex);

	drv_i915_xd9_outputs_darken(&node->outputs, index);

	mutex_unlock(&rd->mutex);
}

/*
 * Maps both buffers uncached into a presenting session's address space:
 * the same one again is a no-op, another is refused with EBUSY.  Returns
 * 0, or the address space's error.
 */
static int
i915_xd9_node_map(
	struct i915_xd9_node *node,
	struct i915_xd9_screen *screen,
	struct i915_ppgtt *vm)
{
	struct i915_gt_object *object;
	uint64_t va;
	uint64_t dma;
	unsigned index;
	unsigned page;
	int error;

	/* Already there, or another space's. */
	if (screen->map_vm == vm)
		return 0;
	if (screen->map_vm != NULL)
		return EBUSY;

	/* Each buffer under the device mutex, which guards the address spaces. */
	mutex_lock(&node->device->mutex);

	error = 0;
	for (index = 0; index < 2U && error == 0; index++) {
		object = screen->buffer[index].obj;
		error = drv_i915_ppgtt_va_alloc(vm, (uint64_t)object->pages * I915_XD9_NODE_PAGE_BYTES, &va);
		if (error != 0)
			break;
		for (page = 0; error == 0 && page < object->pages; page++) {
			error = drv_i915_gt_object_page_dma(object, page, &dma);
			if (error != 0) {
				error = EIO;
				break;
			}

			/* The page, uncached, in order. */
			error = drv_i915_ppgtt_insert_uncached(vm, va + (uint64_t)page * I915_XD9_NODE_PAGE_BYTES, dma, 1U);
		}

		/* The buffer's range. */
		screen->map_va[index] = va;
	}

	/* A buffer that could not be mapped goes back to scratch with the ones before it. */
	if (error != 0) {
		while (index > 0U) {
			index--;
			if (screen->map_va[index] != 0U)
				drv_i915_ppgtt_clear(vm, screen->map_va[index], screen->buffer[index].obj->pages);
			screen->map_va[index] = 0U;
		}

		mutex_unlock(&node->device->mutex);
		return error;
	}

	/* The space holds the buffers from here on. */
	screen->map_vm = vm;

	mutex_unlock(&node->device->mutex);

	/* Succeeded: the GPU can draw into both buffers. */
	return 0;
}

/* Unmaps both buffers from the space that holds them (the ranges are cleared, not reused). */
static void
i915_xd9_node_unmap(
	struct i915_xd9_node *node,
	struct i915_xd9_screen *screen)
{
	unsigned index;

	/* No space holds them. */
	if (screen->map_vm == NULL)
		return;

	/* Both ranges back to scratch under the device mutex. */
	mutex_lock(&node->device->mutex);

	for (index = 0; index < 2U; index++) {
		drv_i915_ppgtt_clear(screen->map_vm, screen->map_va[index], screen->buffer[index].obj->pages);
		screen->map_va[index] = 0U;
	}

	mutex_unlock(&node->device->mutex);

	/* Succeeded: no space holds them. */
	screen->map_vm = NULL;
}

/*
 * Waits until an output's plane shows the surface armed last, reading the
 * live surface every tick.  Returns 0 with the tick, or ETIMEDOUT.
 */
static int
i915_xd9_node_latch(
	struct i915_xd9_node *node,
	unsigned index)
{
	struct i915_xd9_screen *screen;
	uint64_t deadline;
	uint64_t now;
	uint32_t live;

	/* The display the index names. */
	screen = &node->screen[index];

	/* The live surface until it is the armed one, or the time is up. */
	deadline = sched_ticks() + kern_ms_to_ticks(I915_XD9_NODE_LATCH_MS) + 1U;
	for (;;) {
		live = drv_i915_xd9_outputs_live(&node->outputs, index);
		if (live == screen->armed)
			break;
		now = sched_ticks();
		if (now >= deadline) {
			kern_logf("i915: xd9-node: the flip of port %c to 0x%08x did not latch (live 0x%08x)\n", 'A' + (int)node->outputs.output[index].ddi, screen->armed, live);
			return ETIMEDOUT;
		}

		/* A tick's sleep. */
		kern_usleep_range(1000U, 2000U);
	}

	/* Succeeded: the plane shows it from this tick on. */
	screen->latched_tick = sched_ticks();
	screen->armed = 0U;
	return 0;
}

/*
 * Gives the mode set step up after an external display failed: the
 * failure's line, then every external display withdrawn (the hotplug
 * work tells the sessions).  The caller holds the node's mutex.
 */
static void
i915_xd9_node_failed(
	struct i915_xd9_node *node,
	const char *reason,
	int error)
{
	/* The node's step falls below the mode set, with the line that says why. */
	drv_i915_xd9_step_fail(I915_XD9_STEP_MODESET, reason, error);

	/* Succeeded: every external display is dark and gone. */
	i915_xd9_node_withdraw(node);
}

/*
 * Darkens every external display, ends its lease and shows it as without
 * a sink, for the rest of the boot.  The caller holds the node's mutex;
 * the hotplug work moves the events' sequence for it.
 */
static void
i915_xd9_node_withdraw(
	struct i915_xd9_node *node)
{
	unsigned index;
	int step;

	/* Withdrawn once. */
	if (node->withdrawn)
		return;

	/* The hotplug work and the claims see no sink from here on. */
	node->withdrawn = 1;

	/* Each output dark, its buffers given back, its lease ended, its sink forgotten. */
	for (index = 0; index < I915_XD9_OUTPUTS; index++) {
		i915_xd9_node_end(node, index);
		node->outputs.output[index].connected = 0;
	}

	/* No port has a sink as far as the sessions are told. */
	node->outputs.connected = 0U;

	/* Succeeded: the panel's display goes on alone, at the node's step. */
	step = drv_i915_xd9_step();
	kern_logf("i915: xd9-node: the external displays are withdrawn (the display step is %s)\n", drv_i915_xd9_step_name(step));
}
