/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop thread balancer: watches what threads and the person at the
 * machine do and hints the scheduler which kind of core and which priority
 * each user thread should have.  It is not a scheduler: where and when a
 * thread runs stays the scheduler's choice.
 *
 * The notes below are how the input and GPU cores tell the balancer what
 * happened.  In a kernel built without the balancer they are empty.
 */

#ifndef KERN_KERN_DESKTOP_BALANCER_H
#define KERN_KERN_DESKTOP_BALANCER_H

#include <stdint.h>

/*
 * Whether the balancer is built.  The build defines it to 1 when it is;
 * otherwise it is 0 here.
 */
#ifndef CONFIG_KERN_DESKTOP_BALANCER
#define CONFIG_KERN_DESKTOP_BALANCER 0
#endif

/* What a GPU request did, as the GPU core reports it. */
#define KERN_DESKTOP_BALANCER_GPU_SUBMIT	1U	/* work handed to the GPU */
#define KERN_DESKTOP_BALANCER_GPU_PRESENT	2U	/* an image put on a display */

/*
 * When a process last used the GPU, in scheduler ticks (zero: never).
 *
 * It lives in the process and starts at zero with it; a child does not
 * inherit its parent's.  Written by the process's own threads through
 * kern_desktop_balancer_note_gpu(), read by the balancer, both without a
 * lock.
 */
struct desktop_balancer_activity {
	volatile uint64_t gpu_submit_tick;
	volatile uint64_t present_tick;
};

#if CONFIG_KERN_DESKTOP_BALANCER

/*
 * Notes one input event a person made on any device.
 *
 * The input core calls it for the events a device emits, not for the
 * reports it makes up when a device goes or a reader catches up.  Any
 * context, interrupts masked or not: it only stores the time and counts.
 */
void
kern_desktop_balancer_note_input(void);

/*
 * Notes a successful GPU request of the calling thread's process.
 *
 * Thread context, from the GPU core after the request succeeded; what is
 * KERN_DESKTOP_BALANCER_GPU_SUBMIT or KERN_DESKTOP_BALANCER_GPU_PRESENT.
 * It only stores the time.
 */
void
kern_desktop_balancer_note_gpu(
	uint32_t what);

/*
 * Reports when a person last used an input device, in scheduler ticks
 * (zero: not since boot), and how many events they made.  Any context.
 */
void
kern_desktop_balancer_input_activity(
	uint64_t *tick,
	uint64_t *count);

#else

static __inline void
kern_desktop_balancer_note_input(void)
{
}

static __inline void
kern_desktop_balancer_note_gpu(
	uint32_t what)
{
	(void)what;
}

#endif

#endif
