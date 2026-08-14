/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD kernel bring-up bridge.
 */

#include <stddef.h>
#include <stdint.h>

#include "libc/heap.h"
#include "hal/hal.h"
#include "kern/boot.h"
#include "kern/kernel.h"
#include "kern/kmem.h"
#include "kern/net.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/user-probe.h"
#include "kern/syscall.h"

#define KERNEL_HEAP_SIZE (512U * 1024U)
#define SYS_START 0x80000000U

static uint8_t kernel_heap_storage[KERNEL_HEAP_SIZE]
	__attribute__((section(".kernel_heap"), aligned(4096)));
static struct zedbsd_heap kernel_heap;

extern char __low_start[], __low_end[], __high_start[], __high_end[];

void *
kern_malloc(size_t size)
{
	return zedbsd_heap_alloc(&kernel_heap, size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return zedbsd_heap_calloc(&kernel_heap, count, size);
}

void
kern_free(void *pointer)
{
	zedbsd_heap_free(&kernel_heap, pointer);
}

void
kern_memory_get_stats(struct kern_memory_stats *stats)
{
	if (stats == NULL)
		return;
	stats->heap_fixed = KERNEL_HEAP_SIZE;
	stats->heap_current = zedbsd_heap_current_instance(&kernel_heap);
	stats->heap_peak = zedbsd_heap_peak_instance(&kernel_heap);
	stats->heap_largest_free =
		zedbsd_heap_largest_free_instance(&kernel_heap);
	stats->heap_largest_failed =
		zedbsd_heap_largest_failed_instance(&kernel_heap);
	stats->low_image_bytes = (size_t)(__low_end - __low_start);
	stats->high_image_bytes = (size_t)(__high_end - __high_start);
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
	const struct zedbsd_handoff *h = handoff;
	static struct zedbsd_device devices[KERN_PLATFORM_MAX_DEVICES];
	size_t device_count;

	if (h == NULL || h->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (h->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     h->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid zedBSD handoff");
	hal_printf("boot: kernel heap, process, and scheduler initialization\n");
	zedbsd_heap_init_instance(&kernel_heap, kernel_heap_storage,
				 KERNEL_HEAP_SIZE);
	(void)zedbsd_heap_set_active(&kernel_heap);
	hal_set_allocator(kernel_alloc, kernel_free);
	reserve_loaded_image();
	hal_task_init();
	process_init();
	user_probe_init();
	syscall_init();
	sched_init();
	if (process_reaper_start() != 0)
		hal_fatal(__FILE__, __LINE__, "process reaper initialization failed");
	if (net_init() != 0)
		hal_fatal(__FILE__, __LINE__, "network subsystem initialization failed");

	hal_printf("boot: platform device discovery\n");
	device_count = kern_platform_init(h, devices, KERN_PLATFORM_MAX_DEVICES);
	if (device_count == 0)
		hal_fatal(__FILE__, __LINE__, "no boot devices");
	hal_printf("boot: platform devices detected: %u\n",
	    (unsigned)device_count);
	hal_irq_enable();
	kernel_main(h, devices, (unsigned)device_count);
}
