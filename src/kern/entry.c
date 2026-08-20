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
#include "kern/buf.h"
#include "kern/clock.h"
#include "kern/kernel.h"
#include "kern/kmem.h"
#include "kern/klog.h"
#include "kern/net.h"
#include "kern/page.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/user-probe.h"
#include "kern/syscall.h"
#include "kern/sysctl.h"
#include "kern/thread.h"

#define KERNEL_HEAP_SIZE (512U * 1024U)
static uint8_t kernel_heap_storage[KERNEL_HEAP_SIZE]
	__attribute__((section(".kernel_heap"), aligned(ZEDBSD_PAGE_SIZE)));
static struct zedbsd_heap kernel_heap;
static volatile unsigned kernel_heap_lock;

extern char __kernel_vma_start[], __kernel_vma_end[];

static bool
kernel_heap_lock_enter(void)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&kernel_heap_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		hal_compiler_barrier();
	return enabled;
}

static void
kernel_heap_lock_leave(bool enabled)
{
	__atomic_store_n(&kernel_heap_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

void *
kern_malloc(size_t size)
{
	void *result;
	bool enabled = kernel_heap_lock_enter();
	result = zedbsd_heap_alloc(&kernel_heap, size);
	kernel_heap_lock_leave(enabled);
	return result;
}

void *
kern_calloc(size_t count, size_t size)
{
	void *result;
	bool enabled = kernel_heap_lock_enter();
	result = zedbsd_heap_calloc(&kernel_heap, count, size);
	kernel_heap_lock_leave(enabled);
	return result;
}

void
kern_free(void *pointer)
{
	bool enabled = kernel_heap_lock_enter();
	zedbsd_heap_free(&kernel_heap, pointer);
	kernel_heap_lock_leave(enabled);
}

void
kern_memory_get_stats(struct kern_memory_stats *stats)
{
	bool enabled;
	if (stats == NULL)
		return;
	enabled = kernel_heap_lock_enter();
	stats->heap_fixed = KERNEL_HEAP_SIZE;
	stats->heap_current = zedbsd_heap_current_instance(&kernel_heap);
	stats->heap_peak = zedbsd_heap_peak_instance(&kernel_heap);
	stats->heap_largest_free =
		zedbsd_heap_largest_free_instance(&kernel_heap);
	stats->heap_largest_failed =
		zedbsd_heap_largest_failed_instance(&kernel_heap);
	stats->image_bytes = (size_t)(__kernel_vma_end - __kernel_vma_start);
	kernel_heap_lock_leave(enabled);
}

static void *kernel_alloc(size_t size) { return kern_malloc(size); }
static void kernel_free(void *pointer) { kern_free(pointer); }

void
kernel_entry(const void *handoff)
{
	const struct zedbsd_handoff *h = handoff;
	static struct zedbsd_device devices[KERN_PLATFORM_MAX_DEVICES];
	size_t device_count;

	if (h == NULL || h->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (h->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     h->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
	     h->version != ZEDBSD_HANDOFF_VERSION_SUN4U &&
	     h->version != ZEDBSD_HANDOFF_VERSION_X68K) ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid zedBSD handoff");
	kern_log_init();
	kern_logf("boot: kernel heap, process, and scheduler initialization\n");
	zedbsd_heap_init_instance(&kernel_heap, kernel_heap_storage,
				 KERNEL_HEAP_SIZE);
	(void)zedbsd_heap_set_active(&kernel_heap);
	hal_set_allocator(kernel_alloc, kernel_free);
	hal_task_init();
	process_init();
	kern_clock_init();
	user_probe_init();
	syscall_init();
	sched_init();
	sysctl_init();
	if (buf_init() != 0)
		hal_fatal(__FILE__, __LINE__, "buffer cache initialization failed");
	if (thread_prepare_secondaries(hal_cpu_count()) != 0)
		hal_fatal(__FILE__, __LINE__, "secondary thread allocation failed");
	if (hal_cpu_start_others() != HAL_OK)
		hal_fatal(__FILE__, __LINE__, "secondary CPU startup failed");
	if (sched_wait_others_online() != 0)
		hal_fatal(__FILE__, __LINE__, "secondary scheduler startup failed");
	thread_attach_secondaries();
	/* Synchronize the shared kernel translation domain with newly ready CPUs. */
	hal_page_flush_tlb_range(HAL_SPACE_SYS, __kernel_vma_start,
	    ZEDBSD_PAGE_SIZE);
	if (kern_cpu_notify_probe() != HAL_OK)
		hal_fatal(__FILE__, __LINE__, "secondary CPU notification failed");
	hal_printf("boot: HAL initialized successfully. "
	    "[cpu %u, memory %uMB, timer %ums]\n",
	    hal_cpu_count(),
	    (unsigned)(hal_pmem_get_total_size() / (1024U * 1024U)),
	    (unsigned)(1000U / HAL_TIMER_FREQUENCY));
	kern_logf("boot: CPUs ready: %u\n", hal_cpu_count());
	if (process_reaper_start() != 0)
		hal_fatal(__FILE__, __LINE__, "process reaper initialization failed");
	if (net_init() != 0)
		hal_fatal(__FILE__, __LINE__, "network subsystem initialization failed");

	kern_logf("boot: platform device discovery\n");
	device_count = kern_platform_init(h, devices, KERN_PLATFORM_MAX_DEVICES);
	if (device_count == 0)
		hal_fatal(__FILE__, __LINE__, "no boot devices");
	kern_logf("boot: platform devices detected: %u\n",
	    (unsigned)device_count);
	hal_irq_enable();
	/* Deferred device work may submit interrupt-driven I/O. */
	kern_platform_refresh_devices(devices, device_count);
	kernel_main(h, devices, (unsigned)device_count);
}

void
kernel_secondary_entry(hal_cpu_id_t cpu)
{
	if (cpu == 0 || cpu != hal_cpu_current())
		hal_fatal(__FILE__, __LINE__, "invalid secondary CPU entry");
	thread_init_secondary(cpu);
	sched_secondary_init(cpu);
}
