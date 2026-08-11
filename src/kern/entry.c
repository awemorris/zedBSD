/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Boots kernel bring-up bridge.
 */

#include <stddef.h>
#include <stdint.h>

#include "libc/heap.h"
#include "hal/hal.h"
#include "kern/boot.h"
#include "kern/kernel.h"
#include "kern/kmem.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/user-probe.h"
#include "noct/platform.h"

#define KERNEL_HEAP_SIZE (512U * 1024U)
#define SYS_START 0x80000000U

static uint8_t kernel_heap_storage[KERNEL_HEAP_SIZE]
	__attribute__((section(".kernel_heap"), aligned(4096)));
static struct boots_heap kernel_heap;

extern char __low_start[], __low_end[], __high_start[], __high_end[];

void *
kern_malloc(size_t size)
{
	return boots_heap_alloc(&kernel_heap, size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return boots_heap_calloc(&kernel_heap, count, size);
}

void
kern_free(void *pointer)
{
	boots_heap_free(&kernel_heap, pointer);
}

static void *kernel_alloc(size_t size) { return kern_malloc(size); }
static void kernel_free(void *pointer) { kern_free(pointer); }

static void
reserve_loaded_image(void)
{
	uintptr_t low_start = (uintptr_t)__low_start & ~SYS_START;
	uintptr_t low_end = (uintptr_t)__low_end & ~SYS_START;
	uintptr_t high_start = (uintptr_t)__high_start & ~SYS_START;
	uintptr_t high_end = (uintptr_t)__high_end & ~SYS_START;

	pmem_reserve((hal_physaddr_t)low_start, low_end - low_start);
	pmem_reserve((hal_physaddr_t)high_start, high_end - high_start);
}

void
kernel_entry(const void *handoff)
{
	const struct boots_handoff *h = handoff;
	static struct boots_device devices[KERN_PLATFORM_MAX_DEVICES];
	size_t device_count;

	if (h == NULL || h->magic != BOOTS_HANDOFF_MAGIC || h->version != 2 ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid Boots handoff");
	boots_heap_init_instance(&kernel_heap, kernel_heap_storage,
				 KERNEL_HEAP_SIZE);
	(void)boots_heap_set_active(&kernel_heap);
	hal_set_allocator(kernel_alloc, kernel_free);
	reserve_loaded_image();
	if (!boots_noct_prepare_memory())
		hal_fatal(__FILE__, __LINE__, "unable to reserve Noct arena");
	hal_task_init();
	process_init();
	user_probe_init();
	sched_init();

	device_count = kern_platform_init(h, devices, KERN_PLATFORM_MAX_DEVICES);
	if (device_count == 0)
		hal_fatal(__FILE__, __LINE__, "no boot devices");
	hal_irq_enable();
	kernel_main(h, devices, (unsigned)device_count);
}
