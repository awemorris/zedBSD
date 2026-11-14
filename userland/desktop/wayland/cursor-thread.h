/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cursor thread (cursor-thread.c, ws213-p004; the design is
 * plan/ws213/phase001/phase.md 6.2).
 *
 * The cursor thread presents the cursor plane: it reads the cursor's box
 * (the place the input thread writes, the image the main thread chooses)
 * and presents the plane's image at the latest place, once a refresh at
 * most (ws213-p001 C0), whatever the main thread is doing.  It owns every
 * Vulkan object of the plane's presentation (its swapchain, its own
 * command pool, command buffers, fences and semaphores) and waits on its
 * own fences only.  The main thread makes those objects before the thread
 * starts and destroys them after it stops; a failure stops the thread's use
 * of Vulkan and is told to the main thread, which composes the cursor
 * again and closes the plane.
 */

#ifndef KWL_CURSOR_THREAD_H
#define KWL_CURSOR_THREAD_H

#include "compose.h"
#include "cursor-box.h"

#include <pthread.h>
#include <stdint.h>

/* How many images and acquisitions the plane's swapchain uses at most. */
#define KWL_CURSOR_THREAD_IMAGES	4U

/*
 * How long the cursor thread waits for one of its copies when it stops, in
 * nanoseconds (100 ms): a copy that does not finish by then keeps the
 * plane's objects alive (stop_failed).  While it presents, the thread waits
 * for each copy until it is done.
 */
#define KWL_CURSOR_FENCE_WAIT_NS	100000000ULL

/* What one step of the cursor thread did. */
enum kwl_cursor_step {
	/* Presented the plane. */
	KWL_CURSOR_STEP_PRESENTED = 1,
	/* Nothing to present now: wait for a change, or until the deadline given. */
	KWL_CURSOR_STEP_IDLE,
	/* Parked: no Vulkan until the park ends. */
	KWL_CURSOR_STEP_PARKED,
	/* Failed before: no Vulkan again. */
	KWL_CURSOR_STEP_FAILED,
	/* Asked to stop. */
	KWL_CURSOR_STEP_STOP
};

/*
 * What the cursor thread did, counted by the thread alone and read after it
 * ended (it prints them as its last line): presents and copies, its waits
 * for a copy's fence (each until the copy is done), its acquires that found
 * an image at once and those that waited for one, and its waits for the
 * box: until the next change (sleeps) or until a held move is due (holds,
 * the only waits with a deadline).
 */
struct kwl_cursor_counts {
	uint64_t presents;
	uint64_t copies;
	uint64_t copy_waits;
	uint64_t acquires_now;
	uint64_t acquires_waiting;
	uint64_t sleeps;
	uint64_t holds;
};

/*
 * The cursor thread and the plane's presentation it owns.
 *
 * The device and queue are the compositor's (the queue is shared with the
 * frames: each submit and present is made under the compose's queue lock,
 * queue_lock, as Vulkan's external synchronization wants); every other
 * object is the thread's.  held says which cursor image (its address, generation and
 * version) each swapchain image holds.  source, generation, version, x and
 * y are what was presented last, presented_us when that present started.
 * hold_us is how long a move of the same image waits after a present (a
 * refresh less the slack of the clock).  sequence is the box's change count
 * the last step saw.  log prints a line for each present (the compositor's
 * frame log).  failed and stop_failed say the thread failed, or its last
 * copies did not finish in time when it stopped; wake_main is the main
 * thread's wake pipe; counts are the thread's own.
 */
struct kwl_cursor_thread {
	struct kwl_cursor_box *box;
	VkInstance instance;
	VkDevice device;
	VkQueue queue;
	pthread_mutex_t *queue_lock;
	struct vkdemo_display output;
	VkCommandPool pool;
	VkCommandBuffer commands[KWL_CURSOR_THREAD_IMAGES];
	VkFence fences[KWL_CURSOR_THREAD_IMAGES];
	VkSemaphore acquired[KWL_CURSOR_THREAD_IMAGES + 1U];
	VkSemaphore ready[KWL_CURSOR_THREAD_IMAGES];
	unsigned next;
	unsigned held_valid[KWL_CURSOR_THREAD_IMAGES];
	const struct kwl_import *held[KWL_CURSOR_THREAD_IMAGES];
	uint64_t held_generation[KWL_CURSOR_THREAD_IMAGES];
	uint32_t held_version[KWL_CURSOR_THREAD_IMAGES];
	const struct kwl_import *copies[KWL_CURSOR_COPIES];
	unsigned presented;
	uint64_t presented_us;
	const struct kwl_import *source;
	uint64_t generation;
	uint32_t version;
	int32_t x;
	int32_t y;
	uint64_t hold_us;
	uint64_t sequence;
	unsigned log;
	unsigned failed;
	unsigned stop_failed;
	int wake_main;
	struct kwl_cursor_counts counts;
	pthread_t thread;
	unsigned started;
};

int kwl_cursor_thread_start(struct kwl_cursor_thread *cursor);
int kwl_cursor_thread_stop(struct kwl_cursor_thread *cursor);
enum kwl_cursor_step kwl_cursor_thread_step(struct kwl_cursor_thread *cursor, uint64_t *deadline_us);
void kwl_cursor_thread_finish(struct kwl_cursor_thread *cursor);
int kwl_cursor_thread_park(struct kwl_cursor_box *box, unsigned park, unsigned wait_ms);

#endif
