/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel entry points and the kernel heap.
 *
 * kernel_entry() validates the boot handoff, brings up the core subsystems
 * in dependency order, starts the secondary CPUs, discovers the platform
 * devices, and hands over to kernel_main().  Small allocations come from a
 * fixed heap in the kernel image and large ones from page-backed physical
 * memory, both under one lock domain shared with libc's malloc.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libc/heap.h"
#include "hal/hal.h"
#include "kern/io-pool.h"
#include "kern/cache-memory.h"
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

struct kernel_large_allocation {
	struct kernel_large_allocation *next;
	void *pointer;
	struct kern_pmem memory;
};

static uint8_t kernel_heap_storage[KERNEL_HEAP_SIZE]
    __attribute__((section(".kernel_heap"), aligned(ZEDBSD_PAGE_SIZE)));
static struct heap_allocator kernel_heap;
static atomic_uint_t kernel_heap_lock;
static uint8_t kernel_heap_libc_lock_active[HAL_CPU_MAX];
static uint8_t kernel_heap_libc_irq_enabled[HAL_CPU_MAX];
static struct kernel_large_allocation *kernel_large_allocations;

#ifdef ZEDBSD_KERNEL_HEAP_TRACE
/* Private QMP-readable provenance; all writers hold kernel_heap_lock. */
struct kernel_heap_trace_entry {
	uint64_t sequence;
	uintptr_t caller;
	uintptr_t pointer;
	size_t size;
	unsigned cpu;
	unsigned event;
};
static volatile struct kernel_heap_trace_entry kernel_heap_trace[2048];
static uint64_t kernel_heap_trace_sequence;
static volatile unsigned kernel_heap_trace_failed;
static void kernel_heap_trace_record(unsigned event, void *caller, void *pointer, size_t size);
static void kernel_heap_trace_check(unsigned event, void *caller);
static void kernel_heap_trace_observer(void *context, void *pointer, size_t size, enum heap_event event);
#define KERNEL_HEAP_TRACE(event, caller, pointer, size) \
	kernel_heap_trace_record(event, caller, pointer, size)
#define KERNEL_HEAP_CHECK(event, caller) kernel_heap_trace_check(event, caller)
#else
#define KERNEL_HEAP_TRACE(event, caller, pointer, size) ((void)0)
#define KERNEL_HEAP_CHECK(event, caller) ((void)0)
#endif

#ifdef ZEDBSD_KERNEL_HEAP_TRACE
void
__heap_trace_pointer_walk(void *pointer, void *caller)
{
	kernel_heap_trace_record(12U, caller, pointer, 0);
}

#endif

extern char __kernel_vma_start[], __kernel_vma_end[];

static bool kernel_heap_lock_enter(void);
static void kernel_heap_lock_leave(bool enabled);

/*
 * Takes the kernel heap lock on behalf of libc's malloc.
 *
 * libc's malloc/free compatibility entry points use the same active heap
 * as kern_malloc/kern_free.  The weak libc hooks are intentionally no-ops
 * for single-threaded freestanding consumers, so the kernel overrides them
 * and joins the one kernel-heap lock domain.  The interrupt state is kept
 * per CPU because libc gives the unlock no argument to carry it.
 */
void
__libc_heap_lock(
	void)
{
	hal_cpu_id_t cpu;
	bool enabled;

	enabled = hal_irq_disable();

	/* Traps on a recursive lock, which would deadlock below. */
	cpu = hal_cpu_current();
	if (cpu >= HAL_CPU_MAX || kernel_heap_libc_lock_active[cpu] != 0)
		HAL_FATAL("recursive libc kernel heap lock");

	/* Spins for the lock, then records the interrupt state for the unlock. */
	while (!atomic_try_acquire_zero(&kernel_heap_lock))
		hal_compiler_barrier();
	KERNEL_HEAP_CHECK(1, __builtin_return_address(0));
	kernel_heap_libc_irq_enabled[cpu] = enabled ? 1U : 0U;
	kernel_heap_libc_lock_active[cpu] = 1U;
}

/*
 * Releases the kernel heap lock on behalf of libc's free.
 */
void
__libc_heap_unlock(
	void)
{
	hal_cpu_id_t cpu;
	bool enabled;

	/* Traps on an unlock without a matching lock. */
	cpu = hal_cpu_current();
	if (cpu >= HAL_CPU_MAX || kernel_heap_libc_lock_active[cpu] == 0)
		HAL_FATAL("unbalanced libc kernel heap unlock");

	/* Releases the lock and restores the interrupt state saved by the lock. */
	enabled = kernel_heap_libc_irq_enabled[cpu] != 0;
	kernel_heap_libc_lock_active[cpu] = 0;
	kernel_heap_libc_irq_enabled[cpu] = 0;
	KERNEL_HEAP_CHECK(2, __builtin_return_address(0));
	atomic_store_release(&kernel_heap_lock, 0U);
	if (enabled)
		hal_irq_enable();
}

/*
 * Allocates kernel memory.
 *
 * Small requests are served from the fixed heap; large ones, and small ones
 * the fragmented heap cannot serve, get page-backed physical memory with a
 * hidden header that records the allocation for kern_free().
 */
void *
kern_malloc(
	size_t size)
{
	struct kernel_large_allocation *large;
	struct kern_pmem memory;
	void *result;
	size_t header_size;
	bool enabled;

	/* Tries the fixed heap first for a small request. */
	if (size < KERNEL_LARGE_THRESHOLD) {
		enabled = kernel_heap_lock_enter();
		KERNEL_HEAP_TRACE(3, __builtin_return_address(0), NULL, size);
		result = heap_allocator_alloc(&kernel_heap, size);
		kernel_heap_lock_leave(enabled);
		if (result != NULL)
			return result;

		/*
		 * The fixed heap is deliberately small and can become
		 * fragmented.  A failed sub-threshold allocation must still be
		 * allowed to use a page-backed allocation while physical memory
		 * remains available.
		 */
	}

	/* Sizes the hidden header and rejects a request that overflows with it. */
	header_size = (sizeof(*large) + KERNEL_ALLOCATION_ALIGNMENT - 1U) &
		      ~(size_t)(KERNEL_ALLOCATION_ALIGNMENT - 1U);
	if (size > SIZE_MAX - header_size)
		return NULL;

	/* Allocates page-aligned physical memory for the header and the block. */
	memory.size = size + header_size;
	if (hal_pmem_alloc(memory.size, ZEDBSD_PAGE_SIZE,
			   &memory.paddr) != HAL_OK)
		return NULL;

	/* Fills the header and links it into the large allocation list. */
	large = hal_pmem_to_kernel(memory.paddr);
	memset(large, 0, header_size);
	large->pointer = (uint8_t *)hal_pmem_to_kernel(memory.paddr) +
	    header_size;
	large->memory = memory;
	enabled = kernel_heap_lock_enter();
	large->next = kernel_large_allocations;
	kernel_large_allocations = large;
	kernel_heap_lock_leave(enabled);
	result = large->pointer;

	/* Reports the block after the header. */
	return result;
}

/*
 * Allocates zeroed kernel memory for an array.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *result;
	size_t total;

	/* Rejects an array whose total size overflows. */
	if (count != 0 && size > SIZE_MAX / count)
		return NULL;

	/* Allocates and clears the array. */
	total = count * size;
	result = kern_malloc(total);
	if (result != NULL)
		memset(result, 0, total);

	/* Reports the array, or none. */
	return result;
}

/*
 * Frees kernel memory from either allocator.
 *
 * A pointer inside the fixed heap goes back to it; anything else must be a
 * recorded large allocation, and freeing an unknown pointer is fatal.
 */
void
kern_free(
	void *pointer)
{
	struct kernel_large_allocation **link;
	struct kernel_large_allocation *large;
	struct kern_pmem memory;
	uintptr_t address;
	bool enabled;

	large = NULL;
	address = (uintptr_t)pointer;

	/* Ignores a null pointer. */
	if (pointer == NULL)
		return;

	/* Returns a fixed heap block to the heap. */
	enabled = kernel_heap_lock_enter();
	KERNEL_HEAP_TRACE(4, __builtin_return_address(0), pointer, 0);
	if (address >= (uintptr_t)kernel_heap.begin &&
	    address < (uintptr_t)kernel_heap.end) {
		heap_allocator_free(&kernel_heap, pointer);
		kernel_heap_lock_leave(enabled);
		return;
	}

	/* Unlinks the large allocation that owns the pointer. */
	for (link = &kernel_large_allocations; *link != NULL;
	     link = &(*link)->next) {
		if ((*link)->pointer == pointer) {
			large = *link;
			*link = large->next;
			break;
		}
	}

	if (large != NULL)
		memory = large->memory;

	kernel_heap_lock_leave(enabled);

	/* Releases the physical memory outside the lock. */
	if (large == NULL)
		HAL_FATAL("invalid kernel allocation free");
	if (hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
		HAL_FATAL("kernel large allocation free failed");
}

/*
 * Reports the kernel heap and image statistics.
 */
void
kern_memory_get_stats(
	struct kern_memory_stats *stats)
{
	bool enabled;

	/* Ignores a missing result. */
	if (stats == NULL)
		return;

	/* Samples the heap under its lock. */
	enabled = kernel_heap_lock_enter();
	stats->heap_fixed = KERNEL_HEAP_SIZE;
	stats->heap_current = heap_allocator_current(&kernel_heap);
	stats->heap_peak = heap_allocator_peak(&kernel_heap);
	stats->heap_largest_free = heap_allocator_largest_free(&kernel_heap);
	stats->heap_largest_failed =
	    heap_allocator_largest_failed(&kernel_heap);
	stats->image_bytes = (size_t)(__kernel_vma_end - __kernel_vma_start);
	kernel_heap_lock_leave(enabled);
}

/*
 * Enters the kernel from the boot loader on the boot CPU.
 *
 * The subsystems come up in dependency order, the secondary CPUs are
 * started and joined to the scheduler, and platform device discovery runs
 * before kernel_main() takes over.  Any failure is fatal.
 */
void
kernel_entry(
	const void *handoff)
{
	static struct boot_device devices[KERN_PLATFORM_MAX_DEVICES];
	const struct boot_handoff *h;
	size_t device_count;

	/* Refuses a handoff that is missing, foreign, or truncated. */
	h = handoff;
	if (h == NULL ||
	    h->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (h->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     h->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
	     h->version != ZEDBSD_HANDOFF_VERSION_SUN4U &&
	     h->version != ZEDBSD_HANDOFF_VERSION_X68K) ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid zedBSD handoff");

	/* Brings up the log, the heap, and the core subsystems. */
	kern_log_init();
	kern_logf("boot: kernel heap, process, and scheduler initialization\n");
	heap_allocator_init(&kernel_heap, kernel_heap_storage,
			    KERNEL_HEAP_SIZE);
#ifdef ZEDBSD_KERNEL_HEAP_TRACE
	heap_allocator_set_observer(&kernel_heap, kernel_heap_trace_observer, NULL);
#endif
	(void)heap_active_set(&kernel_heap);
	if (hal_task_create_for_init_context() == NULL)
		hal_fatal(__FILE__, __LINE__,
			  "initial task allocation failed");
	process_init();
	kern_clock_init();
	user_probe_init();
	syscall_init();
	sched_init();
	sysctl_init();
	cache_memory_init();
	if (buf_init() != 0)
		hal_fatal(__FILE__, __LINE__,
			  "buffer cache initialization failed");

	/* Starts the secondary CPUs and joins them to the scheduler. */
	if (thread_prepare_secondaries(hal_cpu_count()) != 0)
		hal_fatal(__FILE__, __LINE__,
			  "secondary thread allocation failed");
	if (hal_cpu_start_others() != HAL_OK)
		hal_fatal(__FILE__, __LINE__, "secondary CPU startup failed");
	if (sched_wait_others_online() != 0)
		hal_fatal(__FILE__, __LINE__,
			  "secondary scheduler startup failed");
	thread_attach_secondaries();

	/* Builds nonblocking I/O scratch before mounting filesystems or loading init. */
	io_pool_init();
	if (cache_worker_init() != 0)
		kern_logf("cache: worker scratch unavailable; writeback remains disabled\n");

	/* Synchronizes the shared kernel translation domain with the new CPUs. */
	hal_space_flush_tlb_range(HAL_SPACE_SYS, __kernel_vma_start,
				 ZEDBSD_PAGE_SIZE);
	if (kern_cpu_notify_probe() != HAL_OK)
		hal_fatal(__FILE__, __LINE__,
			  "secondary CPU notification failed");
	hal_printf("boot: HAL initialized successfully. "
		   "[cpu %u, memory %uMB, timer %ums]\n",
		   hal_cpu_count(),
		   (unsigned)(hal_pmem_get_total_size() / (1024U * 1024U)),
		   (unsigned)(1000U / HAL_TIMER_FREQUENCY));
	kern_logf("boot: CPUs ready: %u\n", hal_cpu_count());

	/* Starts the reaper and the network stack. */
	if (process_reaper_start() != 0)
		hal_fatal(__FILE__, __LINE__,
			  "process reaper initialization failed");
	if (net_init() != 0)
		hal_fatal(__FILE__, __LINE__,
			  "network subsystem initialization failed");

	/* Discovers the platform devices. */
	kern_logf("boot: platform device discovery\n");
	device_count =
	    kern_platform_init(h, devices, KERN_PLATFORM_MAX_DEVICES);
	kern_logf("boot: platform devices detected: %u\n",
		  (unsigned)device_count);

	/* Enables interrupts before deferred device work that needs them. */
	hal_irq_enable();
	kern_platform_refresh_devices(devices, device_count);

	/* Hands over to the kernel proper. */
	kernel_main(h, devices, (unsigned)device_count);
}

/*
 * Enters the kernel on a secondary CPU.
 */
void
kernel_secondary_entry(
	hal_cpu_id_t cpu)
{
	/* Refuses the boot CPU or a CPU that is not the caller. */
	if (cpu == 0 || cpu != hal_cpu_current())
		hal_fatal(__FILE__, __LINE__, "invalid secondary CPU entry");

	/* Joins the thread system and the scheduler. */
	thread_init_secondary(cpu);
	sched_secondary_init(cpu);
}

/* Disables interrupts and takes the kernel heap lock. */
static bool
kernel_heap_lock_enter(
	void)
{
	bool enabled;

	enabled = hal_irq_disable();

	/* Spins for the lock. */
	while (!atomic_try_acquire_zero(&kernel_heap_lock))
		hal_compiler_barrier();
	KERNEL_HEAP_CHECK(5, __builtin_return_address(0));

	/* Reports whether interrupts were enabled. */
	return enabled;
}

/* Releases the kernel heap lock and restores the interrupt state. */
static void
kernel_heap_lock_leave(
	bool enabled)
{
	KERNEL_HEAP_CHECK(6, __builtin_return_address(0));
	atomic_store_release(&kernel_heap_lock, 0U);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled)
		hal_irq_enable();
}

#ifdef ZEDBSD_KERNEL_HEAP_TRACE
/* Record without allocation, logging, or any second lock domain. */
static void
kernel_heap_trace_record(
	unsigned event,
	void *caller,
	void *pointer,
	size_t size)
{
	volatile struct kernel_heap_trace_entry *entry;
	uint64_t sequence;

	sequence = ++kernel_heap_trace_sequence;
	entry = &kernel_heap_trace[(sequence - 1U) % 2048U];
	entry->sequence = sequence;
	entry->caller = (uintptr_t)caller;
	entry->pointer = (uintptr_t)pointer;
	entry->size = size;
	entry->cpu = hal_cpu_current();
	entry->event = event;
}

static void
kernel_heap_trace_check(
	unsigned event,
	void *caller)
{
	kernel_heap_trace_record(event, caller, NULL, 0);
	if (!heap_allocator_trace_validate(&kernel_heap) || kernel_heap.errors != 0) {
		kernel_heap_trace_failed = event;
		/* Preserve the first failure in RAM without recursive diagnostics. */
		hal_cpu_panic_all();
	}
}

static void
kernel_heap_trace_observer(
	void *context,
	void *pointer,
	size_t size,
	enum heap_event event)
{
	(void)context;
	kernel_heap_trace_record(10U + (unsigned)event,
	    __builtin_return_address(0), pointer, size);
}
#endif

/*
 * Allocates memory for the HAL. Declared by the HAL interface, so the
 * HAL calls it directly instead of receiving a registered callback.
 */
void *
kernel_alloc(
	size_t size)
{
	void *result;

	result = kern_malloc(size);

	/* Reports the allocation. */
	return result;
}

/*
 * Releases memory for the HAL.
 */
void
kernel_free(
	void *pointer)
{
	kern_free(pointer);
}
