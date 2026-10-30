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
 * in dependency order, starts the secondary CPUs, and hands device discovery
 * over to kernel_main()'s worker. Small allocations come from a
 * fixed heap in the kernel image and large ones from page-backed physical
 * memory, both under the one kernel heap lock.
 */

#include <stddef.h>
#include <stdint.h>
#include <kern/kcrt.h>

#include "heap.h"
#include "hal/hal.h"
#include "kern/io-pool.h"
#include "kern/cache-memory.h"
#include "kern/boot.h"
#include "kern/atomic.h"
#include "kern/buf.h"
#include "kern/clock.h"
#include "kern/cpuperf.h"
#include "kern/kernel.h"
#include "kern/kmem.h"
#include "kern/klog.h"
#include "kern/net.h"
#include "kern/page.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/random.h"
#include "kern/sched.h"
#include "kern/sched_private.h"
#include "kern/user-probe.h"
#include "kern/syscall.h"
#include "kern/sysctl.h"
#include "kern/thread.h"
#include "kern/thread_private.h"

/*
 * The fixed kernel heap.  A small allocation that does not fit falls back
 * to a whole physical page, so on a machine with memory to spare the heap
 * is sized to keep page tables' records and similar small objects out of
 * that fallback.  Smaller machines keep the old size.
 */
#if defined(__x86_64__)
#define KERNEL_HEAP_SIZE (4U * 1024U * 1024U)
#else
#define KERNEL_HEAP_SIZE (512U * 1024U)
#endif
#define KERNEL_LARGE_THRESHOLD (2U * KERN_PAGE_SIZE)
#define KERNEL_ALLOCATION_ALIGNMENT 16U

/* Marks a live large allocation's header, so kern_free() finds it without a search. */
#define KERNEL_LARGE_MAGIC 0x6b6c617267656d6bULL

struct kernel_large_allocation {
	struct kernel_large_allocation *next;
	struct kernel_large_allocation *prev;
	void *pointer;
	uint64_t magic;
	struct kern_pmem memory;
};

/* The hidden header's size: the block starts this far into its first page. */
#define KERNEL_LARGE_HEADER \
	((sizeof(struct kernel_large_allocation) + KERNEL_ALLOCATION_ALIGNMENT - 1U) & \
	 ~(size_t)(KERNEL_ALLOCATION_ALIGNMENT - 1U))

static uint8_t kernel_heap_storage[KERNEL_HEAP_SIZE]
    __attribute__((section(".kernel_heap"), aligned(KERN_PAGE_SIZE)));
static struct kern_heap kernel_heap;
static atomic_uint_t kernel_heap_lock;
static struct kernel_large_allocation *kernel_large_allocations;

#ifdef KERN_KERNEL_HEAP_TRACE
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
static void kernel_heap_trace_observer(void *context, void *pointer, size_t size, enum kern_heap_event event);
#define KERNEL_HEAP_TRACE(event, caller, pointer, size) \
	kernel_heap_trace_record(event, caller, pointer, size)
#define KERNEL_HEAP_CHECK(event, caller) kernel_heap_trace_check(event, caller)
#else
#define KERNEL_HEAP_TRACE(event, caller, pointer, size) ((void)0)
#define KERNEL_HEAP_CHECK(event, caller) ((void)0)
#endif

#ifdef KERN_KERNEL_HEAP_TRACE
/*
 * Records a pointer the kernel heap is about to release.
 */
void
kern_heap_trace_pointer_walk(
	void *pointer,
	void *caller)
{
	kernel_heap_trace_record(12U, caller, pointer, 0);
}

#endif

extern char __kernel_vma_start[], __kernel_vma_end[];

static bool kernel_heap_lock_enter(void);
static void kernel_heap_lock_leave(bool enabled);

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
		result = kern_heap_alloc(&kernel_heap, size);
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
	header_size = KERNEL_LARGE_HEADER;
	if (size > SIZE_MAX - header_size)
		return NULL;

	/* Allocates page-aligned physical memory for the header and the block. */
	memory.size = size + header_size;
	if (hal_pmem_alloc(memory.size, KERN_PAGE_SIZE,
			   &memory.paddr) != HAL_OK)
		return NULL;

	/* Fills the header and links it into the large allocation list. */
	large = hal_pmem_to_kernel(memory.paddr);
	kern_memset(large, 0, header_size);
	large->pointer = (uint8_t *)hal_pmem_to_kernel(memory.paddr) +
	    header_size;
	large->memory = memory;
	large->magic = KERNEL_LARGE_MAGIC;
	enabled = kernel_heap_lock_enter();
	large->next = kernel_large_allocations;
	if (large->next != NULL)
		large->next->prev = large;
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
		kern_memset(result, 0, total);

	/* Reports the array, or none. */
	return result;
}

/*
 * Frees kernel memory from either allocator.
 *
 * A pointer inside the fixed heap goes back to it; anything else must be a
 * recorded large allocation, and freeing an unknown pointer is fatal.  A
 * large allocation's header sits just before the block at the start of its
 * page, so it is found without searching the list: a driver that holds many
 * page-backed blocks freed them in time growing with their number.
 */
void
kern_free(
	void *pointer)
{
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
		kern_heap_free(&kernel_heap, pointer);
		kernel_heap_lock_leave(enabled);
		return;
	}

	/* Finds the header of the large allocation that owns the pointer, and unlinks it. */
	if ((address & (KERN_PAGE_SIZE - 1U)) == KERNEL_LARGE_HEADER) {
		large = (struct kernel_large_allocation *)(address - KERNEL_LARGE_HEADER);
		if (large->magic != KERNEL_LARGE_MAGIC || large->pointer != pointer)
			large = NULL;
	}
	if (large != NULL) {
		if (large->prev != NULL)
			large->prev->next = large->next;
		else
			kernel_large_allocations = large->next;
		if (large->next != NULL)
			large->next->prev = large->prev;
		large->magic = 0;
		memory = large->memory;
	}

	kernel_heap_lock_leave(enabled);

	/* An unknown pointer stops the kernel; the line before the stop names it and the code that freed it. */
	if (large == NULL) {
		hal_printf("\nkern_free: %lx is not a kernel allocation (freed from %lx)\n",
			   (unsigned long)address,
			   (unsigned long)(uintptr_t)__builtin_return_address(0));
		HAL_FATAL("invalid kernel allocation free");
	}

	/* Releases the physical memory outside the lock. */
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
	stats->heap_current = kern_heap_current(&kernel_heap);
	stats->heap_peak = kern_heap_peak(&kernel_heap);
	stats->heap_largest_free = kern_heap_largest_free(&kernel_heap);
	stats->heap_largest_failed = kern_heap_largest_failed(&kernel_heap);
	stats->image_bytes = (size_t)(__kernel_vma_end - __kernel_vma_start);
	kernel_heap_lock_leave(enabled);
}

/*
 * Enters the kernel from the boot loader on the boot CPU.
 *
 * The subsystems come up in dependency order, the secondary CPUs are
 * started and joined to the scheduler, and kernel_main() starts a worker
 * for platform device discovery. Any core initialization failure is fatal.
 */
void
kernel_entry(
	const void *handoff)
{
	const struct kern_boot_handoff *h;
	int quiet;

	/* Refuses a handoff that is missing, foreign, or truncated. */
	h = handoff;
	if (h == NULL ||
	    h->magic != KERN_HANDOFF_MAGIC ||
	    (h->version != KERN_HANDOFF_VERSION_MULTIBOOT &&
	     h->version != KERN_HANDOFF_VERSION_X68K) ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid zedBSD handoff");

	/* Brings up the log, the heap, and the core subsystems. */
	kern_log_init();

	/* kmsg=quiet keeps the kernel's messages off the console, for dmesg only. */
	quiet = kern_boot_parameters_token_present(hal_get_arch_handoff("boot.command-line"), "kmsg=quiet");
	if (quiet)
		kern_log_set_quiet(1);
	kern_logf("boot: kernel heap, process, and scheduler initialization\n");
	kern_heap_init(&kernel_heap, kernel_heap_storage, KERNEL_HEAP_SIZE);
#ifdef KERN_KERNEL_HEAP_TRACE
	kern_heap_set_observer(&kernel_heap, kernel_heap_trace_observer, NULL);
#endif
	if (hal_task_create_for_init_context() == NULL)
		hal_fatal(__FILE__, __LINE__,
			  "initial task allocation failed");
	process_init();
	kern_clock_init();
	user_probe_init();
	syscall_init();
	sched_init();
	kern_random_init();
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
				 KERN_PAGE_SIZE);
	if (kern_cpu_notify_probe() != HAL_OK)
		hal_fatal(__FILE__, __LINE__,
			  "secondary CPU notification failed");

	/* Builds the table of the CPUs' performance now that each takes notifications. */
	kern_cpuperf_init();
	kern_logf("boot: HAL initialized successfully. "
		   "[cpu %u, memory %uMB, timer %ums]\n",
		   hal_cpu_count(),
		   (unsigned)(hal_pmem_get_total_size() / (1024U * 1024U)),
		   (unsigned)(kern_clock_resolution_ns() / 1000000U));
	kern_logf("boot: CPUs ready: %u\n", hal_cpu_count());
	kern_log_boot_step("the HAL and the CPUs are ready");

	/* Starts the reaper and the network stack. */
	if (process_reaper_start() != 0)
		hal_fatal(__FILE__, __LINE__,
			  "process reaper initialization failed");
	if (net_init() != 0)
		hal_fatal(__FILE__, __LINE__,
			  "network subsystem initialization failed");

	/* Enables timer and device interrupts before the boot worker can wait. */
	hal_irq_enable();

	/* Leaves blocking device discovery to the kernel's boot worker. */
	kernel_main(h);
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

#ifdef KERN_KERNEL_HEAP_TRACE
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
	if (!kern_heap_trace_validate(&kernel_heap) || kernel_heap.errors != 0) {
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
	enum kern_heap_event event)
{
	(void)context;
	kernel_heap_trace_record(10U + (unsigned)event,
	    __builtin_return_address(0), pointer, size);
}
#endif

/*
 * Kernel console output for the HAL. Stays NULL until the display driver
 * publishes its text layer; hal_putc() falls back to the early console
 * until then.
 */
void (*kernel_putc)(int c);

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
