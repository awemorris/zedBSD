/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop thread balancer's notes from the input and GPU cores.
 *
 * The input core reports every event a person makes and the GPU core
 * every request that puts work on a GPU or an image on a display.  The
 * notes only store a time and count, so they cost the reporting paths an
 * atomic operation or two and take no lock.
 */

#include <kern/desktop-balancer.h>
#include "kern/atomic.h"
#include "kern/clock.h"
#include "kern/process.h"
#include "kern/thread.h"

/*
 * When a person last made an input event, in scheduler ticks, and how
 * many they made since boot.
 *
 * Any context writes them with atomic operations only, from whichever CPU
 * the input arrived on; readers load them atomically.  Zero time means no
 * input since boot.  They live as long as the kernel.
 */
static volatile uint64_t balancer_input_tick;
static volatile uint64_t balancer_input_count;

/*
 * Notes one input event a person made on any device.
 */
void
kern_desktop_balancer_note_input(
	void)
{
	uint64_t now;

	/* Records when, and counts the event. */
	now = clock_ticks();
	atomic_u64_store_release(&balancer_input_tick, now);
	(void)atomic_u64_fetch_add_relaxed(&balancer_input_count, 1U);
}

/*
 * Notes a successful GPU request of the calling thread's process.
 */
void
kern_desktop_balancer_note_gpu(
	uint32_t what)
{
	struct process *process;
	struct thread *thread;
	uint64_t now;

	/* Only a user process's request says anything about how it behaves. */
	thread = curthread;
	if (thread == NULL)
		return;
	process = thread->proc;
	if (process == NULL || process == &process0)
		return;

	/* Records the time against what the request did. */
	now = clock_ticks();
	if (what == KERN_DESKTOP_BALANCER_GPU_PRESENT) {
		atomic_u64_store_release(&process->desktop_balancer.present_tick, now);
	} else if (what == KERN_DESKTOP_BALANCER_GPU_SUBMIT) {
		atomic_u64_store_release(&process->desktop_balancer.gpu_submit_tick, now);
	}
}

/*
 * Reports when a person last used an input device and how many events
 * they made.
 */
void
kern_desktop_balancer_input_activity(
	uint64_t *tick,
	uint64_t *count)
{
	/* Loads the time and the count; each is recent, not one snapshot. */
	*tick = atomic_u64_load_acquire(&balancer_input_tick);
	*count = atomic_u64_load_acquire(&balancer_input_count);
}
