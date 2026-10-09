/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Contiguous allocation ownership shared by the display and render devices. */
#ifndef KERN_DRIVERS_GPU_BCM2711_BUFFER_H
#define KERN_DRIVERS_GPU_BCM2711_BUFFER_H

#include <stdint.h>
#include <kern/lock.h>
#include <kern/pmem.h>

/* Independent references keep one CPU mapping and physical run alive. */
struct bcm2711_buffer {
	struct spinlock guard;
	uint64_t references;
	uint64_t bytes;
	struct kern_pmem memory;
	void *address;
};

int bcm2711_buffer_create(uint64_t bytes, uint64_t limit, size_t alignment, struct bcm2711_buffer **result);
void bcm2711_buffer_retain(struct bcm2711_buffer *buffer);
void bcm2711_buffer_release(struct bcm2711_buffer *buffer);

#endif
