/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel random number generator (src/kern/random.c).
 */

#ifndef KERN_KERN_RANDOM_H
#define KERN_KERN_RANDOM_H

#include <hal/hal.h>
#include <stddef.h>

/* kern_random_read(): wait until the generator is seeded. */
#define KERN_RANDOM_WAIT	0x0001U

/* Seeds the generator from the platform; call once, early in boot. */
void
kern_random_init(void);

/* Mixes the counter's timing of one timer tick into the pool. */
void
kern_random_tick(
	hal_cpu_id_t cpu);

/* Mixes caller-supplied bytes into the pool without crediting them. */
void
kern_random_add(
	const void *data,
	size_t length);

/*
 * Fills a kernel buffer.  With KERN_RANDOM_WAIT it first waits until the
 * generator is seeded; a signal ends the wait with EINTR.  Returns an errno.
 */
int
kern_random_read(
	void *buffer,
	size_t length,
	unsigned flags);

/* Reports whether the generator has been seeded. */
int
kern_random_seeded(void);

#endif
