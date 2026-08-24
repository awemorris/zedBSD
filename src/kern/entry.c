/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD kernel bring-up bridge.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libc/heap.h"
#include "hal/hal.h"
#include "kern/boot.h"
#include "kern/atomic.h"
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
#define KERNEL_LARGE_THRESHOLD (2U * ZEDBSD_PAGE_SIZE)
#define KERNEL_ALLOCATION_ALIGNMENT 16U
static uint8_t kernel_heap_storage[KERNEL_HEAP_SIZE]
	__attribute__((section(".kernel_heap"), aligned(ZEDBSD_PAGE_SIZE)));
static struct heap_allocator kernel_heap;
static atomic_uint_t kernel_heap_lock;

struct kernel_large_allocation {
	struct kernel_large_allocation *next;
	void *pointer;
	struct hal_pmem memory;
};

static struct kernel_large_allocation *kernel_large_allocations;

extern char __kernel_vma_start[], __kernel_vma_end[];

static bool
kernel_heap_lock_enter(void)
{
	bool enabled = hal_irq_disable();
	while (!atomic_try_acquire_zero(&kernel_heap_lock))
		hal_compiler_barrier();
	return enabled;
}

static void
kernel_heap_lock_leave(bool enabled)
{
	atomic_store_release(&kernel_heap_lock, 0U);
	if (enabled)
		hal_irq_enable();
}

void *
kern_malloc(size_t size)
{
	struct kernel_large_allocation *large;
	struct hal_pmem_request request;
	struct hal_pmem memory;
	void *result;
	size_t header_size;
	bool enabled;

	if (size < KERNEL_LARGE_THRESHOLD) {
		enabled = kernel_heap_lock_enter();
		result = heap_allocator_alloc(&kernel_heap, size);
		kernel_heap_lock_leave(enabled);
		if (result != NULL)
			return result;
		/*
		 * The fixed heap is deliberately small and can become fragmented.
		 * A failed sub-threshold allocation must still be allowed to use a
		 * page-backed allocation while physical memory remains available.
		 */
	}
	header_size = (sizeof(*large) + KERNEL_ALLOCATION_ALIGNMENT - 1U) &
	    ~(size_t)(KERNEL_ALLOCATION_ALIGNMENT - 1U);
	if (size > SIZE_MAX - header_size)
		return NULL;
	memset(&request, 0, sizeof(request));
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = size + header_size;
	request.alignment = ZEDBSD_PAGE_SIZE;
	request.type = HAL_PMEM_TYPE_RAM;
	if (hal_pmem_alloc(&request, &memory) != HAL_OK)
		return NULL;
	large = memory.vaddr;
	memset(large, 0, header_size);
	large->pointer = (uint8_t *)memory.vaddr + header_size;
	large->memory = memory;
	enabled = kernel_heap_lock_enter();
	large->next = kernel_large_allocations;
	kernel_large_allocations = large;
	kernel_heap_lock_leave(enabled);
	result = large->pointer;
	return result;
}

void *
kern_calloc(size_t count, size_t size)
{
	void *result;
	size_t total;
	if (count != 0 && size > SIZE_MAX / count)
		return NULL;
	total = count * size;
	result = kern_malloc(total);
	if (result != NULL)
		memset(result, 0, total);
	return result;
}

void
kern_free(void *pointer)
{
	struct kernel_large_allocation **link, *large = NULL;
	struct hal_pmem memory;
	bool enabled;
	uintptr_t address = (uintptr_t)pointer;

	if (pointer == NULL)
		return;
	enabled = kernel_heap_lock_enter();
	if (address >= (uintptr_t)kernel_heap.begin &&
	    address < (uintptr_t)kernel_heap.end) {
		heap_allocator_free(&kernel_heap, pointer);
		kernel_heap_lock_leave(enabled);
		return;
	}
	for (link = &kernel_large_allocations; *link != NULL;
	    link = &(*link)->next)
		if ((*link)->pointer == pointer) {
			large = *link;
			*link = large->next;
			break;
		}
	if (large != NULL) {
		memory = large->memory;
	}
	kernel_heap_lock_leave(enabled);
	if (large == NULL)
		HAL_FATAL("invalid kernel allocation free");
	if (hal_pmem_free(&memory) != HAL_OK)
		HAL_FATAL("kernel large allocation free failed");
}

void
kern_memory_get_stats(struct kern_memory_stats *stats)
{
	bool enabled;
	if (stats == NULL)
		return;
	enabled = kernel_heap_lock_enter();
	stats->heap_fixed = KERNEL_HEAP_SIZE;
	stats->heap_current = heap_allocator_current(&kernel_heap);
	stats->heap_peak = heap_allocator_peak(&kernel_heap);
	stats->heap_largest_free =
		heap_allocator_largest_free(&kernel_heap);
	stats->heap_largest_failed =
		heap_allocator_largest_failed(&kernel_heap);
	stats->image_bytes = (size_t)(__kernel_vma_end - __kernel_vma_start);
	kernel_heap_lock_leave(enabled);
}

static void *kernel_alloc(size_t size) { return kern_malloc(size); }
static void kernel_free(void *pointer) { kern_free(pointer); }

void
kernel_entry(const void *handoff)
{
	const struct boot_handoff *h = handoff;
	static struct boot_device devices[KERN_PLATFORM_MAX_DEVICES];
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
	heap_allocator_init(&kernel_heap, kernel_heap_storage,
				 KERNEL_HEAP_SIZE);
	(void)heap_active_set(&kernel_heap);
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
