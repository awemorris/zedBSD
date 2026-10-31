/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU device of the nvrtx driver (the gpu-device stage, nvrtx.resident=1).
 *
 * After the takeover of the firmware's head (takeover) the GSP stays up and the
 * GPU is registered with the GPU core as a device with storage, blobs in the
 * VRAM, transfers, CPU mappings, sharing of blobs between its opens and one
 * display: the head the firmware lit, in the mode it lit it with, with its
 * refresh boundaries, its cursor plane and its scanout constraints.  node.c
 * holds the sessions and resources, present.c the display's operations,
 * cursor.c the cursor plane, share.c the sharing and the scanout operations.
 */

#ifndef DRIVERS_GPU_NVRTX_NODE_H
#define DRIVERS_GPU_NVRTX_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-scanout.h>
#include <drivers/gpu/gpu-share.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/waitq.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

struct nvrtx_vk_session;

/* The one display's identity and generation. */
#define NVRTX_NODE_DISPLAY_ID		1U
#define NVRTX_NODE_GENERATION		1U

/* The display's topology sequence: its first value, which nothing moves yet (no hotplug events). */
#define NVRTX_NODE_TOPOLOGY_FIRST	1U

/* The markers of supervised jobs the renderer holds at once (runtime.c). */
#define NVRTX_NODE_MARKERS		64U

/*
 * How long a run of a session's may take on the GPU from its submission
 * (or from the end of the last stop, when later) before the recovery looks
 * for what holds the GPU up, and how long it may wait at most from the same
 * start (outside the recovery's stops) before its session is lost even when
 * no stop came.
 */
#define NVRTX_NODE_RUN_US		2000000U
#define NVRTX_NODE_RUN_LIMIT_US		8000000U

/* How long a video decoder's picture is waited for (vk-node.c). */
#define NVRTX_NODE_DECODE_US		2000000U

/* Where a queue entry is: free (a marker), reserved, queued, being run. */
#define NVRTX_ENTRY_FREE		0U
#define NVRTX_ENTRY_RESERVED		1U
#define NVRTX_ENTRY_QUEUED		2U
#define NVRTX_ENTRY_RUNNING		3U
#define NVRTX_ENTRY_PARKED		4U

/* The two kinds of resource: ordinary storage in RAM, a blob in the VRAM. */
#define NVRTX_RESOURCE_STORAGE		1U
#define NVRTX_RESOURCE_BLOB		2U

/*
 * The VRAM of a blob: a buffer object mapped in the CPU's window.
 *
 * Shared by the blob, every import of it into another open and every
 * export the GPU core holds; refs counts them, under the node's lock, and
 * the last one gives the VRAM back.  bytes is the size the blob was asked
 * for.  kept is set when the display could not be moved off the VRAM: it
 * is then never given back (until power-off), though the record goes.
 * cpu_written is set once the CPU may have written it (an application's
 * view, a write of the core, an executor's copy by the CPU), exported once
 * it was exported to another open; both stay.
 *
 * described is set by the first export of the blob as an image, with the
 * image's description: every later export must describe it the same way,
 * and a display shows the blob as described (its layout and rows).  bound
 * is set by the first image of the Vulkan executor bound to the blob's
 * storage, with that image's shape (bound_tiled for block linear, its row
 * pitch, size and offset): an export must describe that shape, and an
 * image bound later to an exported blob must have the described one.
 * All of them under the node's lock; they stay.
 */
struct nvrtx_backing {
	struct nvrtx_bo bo;
	uint64_t bytes;
	uint32_t refs;
	int kept;
	int cpu_written;
	int exported;
	int described;
	struct gpu_image_descriptor image;
	int bound;
	int bound_tiled;
	uint32_t bound_pitch;
	uint32_t bound_width;
	uint32_t bound_height;
	uint64_t bound_offset;
};

/*
 * One resource of a session.
 *
 * Storage is a contiguous run of RAM the CPU reads and writes directly; a
 * presentation copies its rows into a surface of the node.  A blob names a
 * backing in the VRAM, which the display shows as it is; an import is a
 * blob naming another open's backing.  Created by the resource, blob or
 * import operation and freed by the destroy, which first takes a blob off
 * the screen and then drops its reference to the backing.
 */
struct nvrtx_resource {
	uint32_t kind;
	uint64_t bytes;
	struct kern_pmem ram;
	uint8_t *cpu;
	struct nvrtx_backing *backing;
	/* A blob's protocol number and its place in its session's list of blobs (NULL session: not listed). */
	uint32_t resource_id;
	struct nvrtx_session *session;
	struct nvrtx_resource *next_blob;
};

/*
 * One open of the GPU device.
 *
 * blobs lists its blobs by protocol number (the executor reads streams and
 * writes replies there), vk is its executor session, and pending counts
 * its queued frames and its reserved, queued or parked markers (under the
 * node's queue_lock): a drain waits until it is zero.  queued counts only
 * the entries in the worker's queue (a command waits for them, not for the
 * parked markers).  stopping (under queue_lock) is set by the core's stop
 * of the session: it takes no new work.
 *
 * The executor's runs (vk-node.c): run_fence is the work channel's fence
 * of the session's newest run and run_us when it was submitted (both
 * under the node's lock); run_took_us is the time from the submission to
 * the end a wait last saw for one of its runs (atomically, by whoever
 * waited), which the next wait expects; run_lost is set (atomically, by whoever waited)
 * once a run of the session did not end in time: the session puts nothing
 * more on the GPU and its waits fail at once.  quiet_mark is the GPU's
 * submission count after the session's last run and quiet_epoch counts
 * the stretches between other submissions (both under the node's lock).
 *
 * channel is where the session's runs go, made by its first frame (under
 * the node's channel_lock, NULL before): a channel of its own, or the
 * driver's work channel when shared is set (no channel of its own could be
 * made, or the boot asked for none), whose runs all sessions share.
 * quarantined (under queue_lock) is set by the core's isolate: the
 * session's channel and memory are kept the device's (never given back).
 * lane is the renderer's lane its frames and markers go through (set with
 * channel: its number's lane, NULL for the node's main lane).  exec_lock
 * (taken before the node's lock) holds the session's executor for one
 * frame, a blob's attach or the close: the node's lock is let go while a
 * frame waits for one of its runs to end, and the session's executor stays
 * its frame's meanwhile.
 */
struct nvrtx_session {
	struct nvrtx_node *node;
	struct nvrtx_resource *blobs;
	struct nvrtx_vk_session *vk;
	uint32_t pending;
	uint32_t queued;
	int stopping;
	uint32_t run_fence;
	uint64_t run_us;
	uint64_t run_took_us;
	int run_lost;
	uint64_t quiet_mark;
	uint64_t quiet_epoch;
	struct nvrtx_work_channel *channel;
	int shared;
	int quarantined;
	struct nvrtx_lane *lane;
	struct mutex exec_lock;
};

/*
 * One entry of the renderer's queue (runtime.c): a frame (a copy of its
 * wire, allocated with the entry and freed once run) or a marker of a
 * supervised job (one of the node's markers, reused).  error is the result
 * a marker completes with (EIO for one cancelled by a fault).  A parked
 * marker waits for its session's newest run when the worker took it: that
 * run's fence and when it was submitted.
 */
struct nvrtx_node_entry {
	struct nvrtx_node_entry *next;
	uint32_t kind;
	uint32_t state;
	struct nvrtx_session *session;
	struct drv_gpu_completion *completion;
	int error;
	uint32_t bytes;
	uint8_t *wire;
	uint32_t run_fence;
	uint64_t run_us;
};

/*
 * One lane of the renderer (runtime.c): a queue of frames and markers (head
 * to tail) its worker thread takes in order, woken by available, and the
 * markers parked for their runs (parked_head to parked_tail) its completer
 * thread completes in order, woken by parked_more; all under the node's
 * queue_lock.  The node's main lane serves every session that shares the
 * work channel; each session's number on the runlist has a lane of its own,
 * whose threads are made the first time a session takes the number
 * (started) and serve every session that has it, one after the other, for
 * the node's life.
 */
struct nvrtx_lane {
	struct nvrtx_node *node;
	struct nvrtx_node_entry *head;
	struct nvrtx_node_entry *tail;
	struct wait_queue available;
	struct nvrtx_node_entry *parked_head;
	struct nvrtx_node_entry *parked_tail;
	struct wait_queue parked_more;
	int started;
};

/*
 * The counts nvrtx.vk.perf=1 writes into the kernel log every 5 s: runs are
 * the push buffers put on the work channel; ended the runs a wait saw end
 * (the executor's or a parked marker's) and run_us the time from their
 * submission to then; blocked the executor's waits for its runs under the
 * node's lock and blocked_us their time, parked the markers that waited
 * for their runs outside it; copies and copy_bytes the transfers the CPU did
 * through the window; frames the renderer's frames, wait_us the time they
 * waited for the node's lock and held_us the time they held it; flips the
 * presentations and flip_us the time their flips waited for the head.  Whoever
 * does the work adds with relaxed atomics; the message drain takes and clears
 * them.
 *
 * The GPU's side of the runs: timed the runs the GPU's timer measured and
 * gpu_us their time on the GPU, run_max_us and gpu_max_us the longest of each
 * kind; draws and quads the application's and the executor's own draws in them,
 * push_bytes their push buffers.  streams, stream_bytes and stream_us are the
 * command streams copied out of the blobs through the window and the time those
 * copies took.
 *
 * The cursor plane's side: cursor_sets the images the lease set, cursor_same
 * those equal to the image already shown (nothing sent), cursor_set_us the
 * time the sets held the node's lock; cursor_moves the moves and
 * cursor_move_us the time they held it.
 */
struct nvrtx_node_perf {
	uint64_t runs;
	uint64_t ended;
	uint64_t run_us;
	uint64_t run_max_us;
	uint64_t blocked;
	uint64_t blocked_us;
	uint64_t parked;
	uint64_t timed;
	uint64_t gpu_us;
	uint64_t gpu_max_us;
	uint64_t draws;
	uint64_t quads;
	uint64_t push_bytes;
	uint64_t streams;
	uint64_t stream_bytes;
	uint64_t stream_us;
	uint64_t copies;
	uint64_t copy_bytes;
	uint64_t frames;
	uint64_t wait_us;
	uint64_t held_us;
	uint64_t flips;
	uint64_t flip_us;
	uint64_t cursor_sets;
	uint64_t cursor_same;
	uint64_t cursor_set_us;
	uint64_t cursor_moves;
	uint64_t cursor_move_us;
};

/*
 * The registered GPU device and its display.
 *
 * Made once by nvrtx_node_start() and kept until power-off, as the GSP is.
 * lock serializes the lease, the frames and the screen's state; it is taken
 * before the memory lock.  present_lock is taken before lock and held by a
 * presentation throughout, and by the lease's end, a blob's going off the
 * screen and the stop: the window's flips and its notifiers, next and the
 * node's frames are its; a presentation waits for its flip to begin
 * holding it but not lock.  owner is the session holding the lease (NULL when
 * free) and lease its number; sequence counts the frames shown under the
 * lease.  shown is the blob on the screen, NULL while a surface of the node
 * is.  frames are the node's two surfaces storage frames are copied into,
 * next the one the next copy uses; the console surface is takeover's first.
 * broken is set once a flip did not begin, and by the stop for the reboot
 * once the GSP is stopped: nothing touches the display after it.
 *
 * refresh_lock (IRQ-safe, never held across a wait) guards the refresh
 * count: the head's 16-bit frame counter extended to a count that never
 * goes back (refresh_frame is the last reading, refresh_time_ns the time the
 * count last moved); readers take neither the node's lock nor its present
 * lock.
 *
 * The cursor plane, under the node's lock: cursor_object holds two images
 * of 64 by 64 pixels (slots), cursor_ready says the cursor channel and the
 * slots exist, cursor_slot is the slot last written (the one a shown cursor
 * reads), cursor_has_image that the lease set one, cursor_visible that the
 * core shows it, cursor_point the position word last sent.  cursor_pixels
 * holds the CPU's copy of each slot (the words written into it, one slot
 * after the other): the copy of cursor_slot is what the slot holds while
 * cursor_has_image is set, so an image equal to it is not sent again.
 *
 * The renderer (runtime.c, nvrtx.vk=1; renderer is set when it is bound):
 * queue_lock (IRQ-safe) guards the lanes (main_lane, and lanes, one per
 * session's number: their queues and parked markers), the markers, every
 * session's pending and queued counts and faulted; retired wakes the
 * drains.  faulted is set by the GPU
 * core's fault: nothing new is taken and what is queued or parked completes
 * with EIO.
 *
 * The sessions' channels (runtime.c): channel_lock (a mutex, taken before
 * the GSP's lock and never with the node's lock) serializes their making
 * and closing; chid_used has a bit for each session's number on the
 * runlist in use (NVRTX_WORK_SESSION_CHID_FIRST onward), chid_last is the
 * last one handed out (the next is looked for after it, so a number is not
 * reused at once), and channels names the session of each number (under
 * queue_lock as well, for the GSP's error reports).  channels_wanted is set
 * at the start unless the boot asked for no channel of the sessions' own
 * (nvrtx.channels=0).
 *
 * The recovery (runtime.c): the reaper thread looks at the channels every
 * period, or when reaper_wake wakes it, and stops the one that holds the
 * graphics engine up; it holds channel_lock while it looks and stops.
 * kill_end_us is when its last stop ended (atomically; a run's time counts
 * from it when later), kills_wanted is set at the start unless the boot
 * asked for no stop (nvrtx.kill=0), recovery_stuck once a stop left a
 * channel stuck (no other channel is stopped then: the device faults),
 * bus_lost (atomically) once the GSP reported the GPU off the bus,
 * reaper_stop (under queue_lock) by the stop for the reboot, after which
 * the reaper stops nothing, and reaper_busy while it calls the GSP.
 *
 * The message drain (node.c): drain_running is set while its thread runs,
 * drain_stop asks it to end at its next turn (the reboot's stop, which
 * then reads the GSP's replies itself).
 *
 * perf is set at the start when the boot asked for the counts
 * (nvrtx.vk.perf=1) and never changes; counts are the counts, perf_since_us
 * the time the drain last wrote them (the drain's alone).
 */
struct nvrtx_node {
	struct nvrtx_gpu *gpu;
	struct drv_gpu_device *device;
	struct drv_gpu_ops ops;
	struct mutex present_lock;
	struct mutex lock;
	void *owner;
	uint64_t lease;
	uint64_t next_lease;
	uint64_t sequence;
	uint64_t shown_time_ns;
	struct nvrtx_resource *shown;
	struct nvrtx_bo frame_objects[2];
	struct nvrtx_disp_surface frames[2];
	unsigned next;
	uint32_t next_resource_id;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	int broken;
	struct spinlock refresh_lock;
	uint64_t refresh_count;
	uint64_t refresh_time_ns;
	uint32_t refresh_frame;
	int refresh_frame_valid;
	struct nvrtx_bo cursor_object;
	int cursor_ready;
	unsigned cursor_slot;
	int cursor_has_image;
	int cursor_visible;
	uint32_t cursor_point;
	uint32_t *cursor_pixels;
	int renderer;
	struct spinlock queue_lock;
	struct wait_queue retired;
	struct nvrtx_lane main_lane;
	struct nvrtx_lane lanes[NVRTX_WORK_SESSION_CHANNELS];
	struct nvrtx_node_entry markers[NVRTX_NODE_MARKERS];
	int faulted;
	struct mutex channel_lock;
	uint32_t chid_used;
	uint32_t chid_last;
	struct nvrtx_session *channels[NVRTX_WORK_SESSION_CHANNELS];
	int channels_wanted;
	struct wait_queue reaper_wake;
	uint64_t kill_end_us;
	int kills_wanted;
	int recovery_stuck;
	int bus_lost;
	int reaper_stop;
	int reaper_busy;
	volatile int drain_running;
	volatile int drain_stop;
	/*
	 * The display scans out block-linear blobs (nvrtx.disp.bl=1 on a window
	 * with a layout field); otherwise it shows linear rows only.
	 * disp_bl_shown is set once the first block-linear frame was flipped
	 * to (said once in the log); under the node's lock.
	 */
	int disp_bl;
	int disp_bl_shown;
	int perf;
	struct nvrtx_node_perf counts;
	uint64_t perf_since_us;
};

/*
 * Adds to one of the node's counts when the boot asked for them; any
 * context, no lock.
 */
static inline void
nvrtx_node_count(
	const struct nvrtx_node *node,
	uint64_t *count,
	uint64_t amount)
{
	/* Nothing is counted without nvrtx.vk.perf=1. */
	if (node->perf == 0)
		return;

	/* The drain takes the count with an exchange, so the add is never lost. */
	(void)__atomic_fetch_add(count, amount, __ATOMIC_RELAXED);
}

/*
 * Raises one of the node's maxima to a value when the boot asked for the
 * counts; any context, no lock.
 */
static inline void
nvrtx_node_count_max(
	const struct nvrtx_node *node,
	uint64_t *maximum,
	uint64_t value)
{
	uint64_t seen;
	bool replaced;

	/* Nothing is counted without nvrtx.vk.perf=1. */
	if (node->perf == 0)
		return;

	/* Replaces a smaller maximum, trying again when another thread changed it meanwhile. */
	seen = __atomic_load_n(maximum, __ATOMIC_RELAXED);
	while (seen < value) {
		replaced = __atomic_compare_exchange_n(maximum, &seen, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
		if (replaced)
			break;
	}
}

extern const struct drv_gpu_display_ops nvrtx_display_ops;
extern const struct drv_gpu_share_ops nvrtx_share_ops;
extern const struct drv_gpu_scanout_ops nvrtx_scanout_ops;
extern const struct drv_gpu_layout_ops nvrtx_layout_ops;

int nvrtx_present_start(struct nvrtx_node *node);
void nvrtx_present_session_close(struct nvrtx_node *node, void *session);
void nvrtx_present_resource_gone(struct nvrtx_node *node, struct nvrtx_resource *resource);
void nvrtx_present_refresh_sample(struct nvrtx_node *node, uint64_t *count, uint64_t *time_ns);
void nvrtx_present_stop(struct nvrtx_node *node);

void nvrtx_cursor_start(struct nvrtx_node *node);
void nvrtx_cursor_lease_end(struct nvrtx_node *node);
int nvrtx_cursor_set(void *private_data, void *session, void *object, const struct gpu_display_cursor_image *request);
int nvrtx_cursor_move(void *private_data, void *session, const struct gpu_display_cursor_move *request);
int nvrtx_cursor_hide(void *private_data, void *session, const struct gpu_display_cursor_hide *request);

void nvrtx_runtime_bind(struct nvrtx_node *node);
void nvrtx_runtime_start(struct nvrtx_node *node);
void nvrtx_runtime_stop(struct nvrtx_node *node);
void nvrtx_runtime_session_close(struct nvrtx_node *node, struct nvrtx_session *session);
void nvrtx_runtime_reaper_wake(struct nvrtx_node *node);

int nvrtx_vk_node_run_wait(struct nvrtx_session *session, uint32_t fence, uint64_t submitted_us);
bool nvrtx_vk_node_run_passed(struct nvrtx_session *session, uint32_t fence);
int nvrtx_vk_node_blob_attach(struct nvrtx_node *node, struct nvrtx_session *session, uint64_t blob_id, struct nvrtx_backing *backing, uint64_t bytes);

void nvrtx_node_backing_put(struct nvrtx_node *node, struct nvrtx_backing *backing);
int nvrtx_node_backing_cpu(struct nvrtx_node *node, struct nvrtx_backing *backing, volatile uint8_t **cpu);
uint32_t nvrtx_node_resource_id(struct nvrtx_node *node);
void nvrtx_node_blob_listed(struct nvrtx_node *node, struct nvrtx_session *session, struct nvrtx_resource *resource, uint32_t resource_id);
bool nvrtx_node_video_available(struct nvrtx_node *node);

#endif
