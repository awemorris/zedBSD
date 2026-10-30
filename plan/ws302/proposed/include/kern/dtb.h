/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop thread balancer: a kernel thread that watches what threads
 * and the person at the machine do and hints the scheduler which kind of
 * core and which priority each thread should have.  It is not a scheduler:
 * where and when a thread runs stays the scheduler's choice.
 *
 * The notes below are how the input and GPU cores tell the balancer what
 * happened.  In a kernel built without the balancer they are empty.
 */

#ifndef KERN_KERN_DTB_H
#define KERN_KERN_DTB_H

#include <stdint.h>

/*
 * Whether the balancer is built.  The platforms that pass the build's
 * configuration define it; the others build without it.
 */
#ifndef CONFIG_DTB
#define CONFIG_DTB 0
#endif

/* What a GPU request did, as the GPU core reports it. */
#define KERN_DTB_GPU_SUBMIT		1U	/* work handed to the GPU */
#define KERN_DTB_GPU_PRESENT		2U	/* an image put on a display */

/*
 * When a process last used the GPU, in scheduler ticks (zero: never).
 *
 * Written by the process's own threads through kern_dtb_note_gpu(), read
 * by the balancer, both without a lock.
 */
struct dtb_process_activity {
	volatile uint64_t gpu_submit_tick;
	volatile uint64_t present_tick;
};

/*
 * What the balancer remembers of one thread between its passes.
 *
 * Only the balancer's thread reads or writes it, so it needs no lock.
 * run_window holds the ticks the thread ran in each of the last passes,
 * from which the balancer tells a thread that keeps a CPU busy.
 */
struct dtb_thread_state {
	uint64_t run_ticks_seen;
	uint64_t moved_tick;			/* when the balancer last moved it */
	uint32_t syscalls_seen;
	uint16_t run_window[10];
	uint8_t window_next;
	uint8_t quiet_passes;			/* passes in a row that looked not busy */
	uint8_t light_passes;			/* passes in a row that looked light */
	uint8_t marks;				/* DTB_MARK_*, private to dtb.c */
	uint8_t rule;				/* the rule that chose the current hint */
	uint8_t reserved;
};

#if CONFIG_DTB

/*
 * Starts the balancer once the CPU performance framework and the thermal
 * core are up.  Thread context, once.  With the boot parameter dtb=off the
 * thread is not started; the counters are kept either way.
 */
void
kern_dtb_init(void);

/*
 * Notes one input report a person made on any device.
 *
 * The input core calls it for the events a device emits, not for the
 * reports it makes up when a device goes or a reader catches up.  Any
 * context, interrupts masked or not, under any lock: it only stores the
 * time and counts.
 */
void
kern_dtb_note_input(void);

/*
 * Notes a successful GPU request of the calling thread.
 *
 * Thread context, from the GPU core after the request succeeded; what is
 * KERN_DTB_GPU_SUBMIT or KERN_DTB_GPU_PRESENT.  It only stores the time.
 */
void
kern_dtb_note_gpu(
	uint32_t what);

#else

static __inline void
kern_dtb_init(void)
{
}

static __inline void
kern_dtb_note_input(void)
{
}

static __inline void
kern_dtb_note_gpu(
	uint32_t what)
{
	(void)what;
}

#endif

#endif
