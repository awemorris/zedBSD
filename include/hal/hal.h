/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Historical Architecture Library
 *
 * This header defines a kernel porting HAL. A HAL is implemented fo a
 * combination of a CPU architecture and a machine/board type. A HAL
 * doesn't implement basic kernel features such as scheduling
 * algorithm, and only implements low level operations required fo
 * contemporary 32-bit and 64-bit POSIX-compatible kernels.
 */

#ifndef HAL_HAL_H
#define HAL_HAL_H

#include <hal/types.h>
#include <hal/arch.h>
#include <hal/atomic.h>

/*
 * HAL error code.
 */
enum hal_error {
	HAL_OK = 0,
	HAL_ERR_INVALID,
	HAL_ERR_UNSUPPORTED,
	HAL_ERR_BUSY,
	HAL_ERR_NOMEM,
	HAL_ERR_TIMEOUT,
	HAL_ERR_STATE,
	HAL_ERR_IO
};

/*
 * HAL C runtime
 *
 * - Must be used only in HAL and early init phase of the kernel.
 */

#define HAL_ASSERT(e)	((e) ? (void)0 : hal_assert(__FILE__, __LINE__, #e))
#define HAL_FATAL(msg)	hal_fatal(__FILE__, __LINE__, msg)

int
hal_strlen(
	const char *s);

void *
hal_memset(
	void *s,
	int c,
	size_t n);

void *
hal_memset16(
	uint16_t *s,
	uint16_t c,
	size_t n);

void *
hal_memset32(
	uint32_t *s,
	uint32_t c,
	size_t n);

void *
hal_memcpy(
	void *dest,
	const void *src,
	size_t n);

int
hal_putchar(
	int c);

/*
 * Put one character on the early console.
 *
 * Before the kernel console exists this writes to the HAL's own early
 * console. Once the kernel publishes kernel_putc, every character is
 * delegated there instead and the HAL stops touching the display.
 */
void
hal_putc(
	int c);

int
hal_puts(
	const char *s);

int
hal_printf(
	const char *format,
	...);

void
hal_assert(
	const char *file,
	int line,
	const char *exp);

void
hal_fatal(
	const char *file,
	int line,
	const char *s);


/*
 * SMP
 *
 * Secondary processors are initialized inside HAL, and waiting fo
 * IPI at the moment kernel_main() called.
 */

#define HAL_CPU_MAX	512U
#define HAL_CPU_MASK_WORDS	((HAL_CPU_MAX + 63U) / 64U)

typedef uint32_t hal_cpu_id_t;

struct hal_cpu_mask {
	uint64_t bits[HAL_CPU_MASK_WORDS];
};

/*
 * XXX: Add explanation.
 */
int
hal_cpu_start_others(void);

/*
 * Get the numbers of the CPUs.
 */
unsigned
hal_cpu_count(void);

/*
 * Get the ID of the current CPU of the caller context.
 */
hal_cpu_id_t
hal_cpu_current(void);

/*
 * XXX: Add explanation.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result);

/*
 * XXX: Add explanation.
 */
int
hal_cpu_notify(
	hal_cpu_id_t cpu);

/*
 * XXX: Add explanation.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *targets);

/*
 * XXX: Add explanation.
 */
_Noreturn void
hal_cpu_park(void);

/*
 * XXX: Add explanation.
 */
_Noreturn void
hal_cpu_panic_all(void);

/*
 * Atomically enable interrupts and halt, then return with IRQs disabled.
 */
void
hal_cpu_idle(void);

/*
 * XXX: Rename to hal_cpu_halt().
 * XXX: Is this same to hal_cpu_idle()??
 *
 * Halt until a next interrupt.
 */
void
hal_halt(void);

/*
 * Utility to clear all CPU mask bits.
 */
static inline void
hal_cpu_mask_zero(
	struct hal_cpu_mask *mask)
{
	unsigned i;

	for (i = 0; i < HAL_CPU_MASK_WORDS; i++)
		mask->bits[i] = 0;
}

/*
 * Utility to set all CPU mask bits.
 */
static inline void
hal_cpu_mask_fill(
	struct hal_cpu_mask *mask)
{
	unsigned i;

	for (i = 0; i < HAL_CPU_MASK_WORDS; i++)
		mask->bits[i] = ~(uint64_t)0;
}

/*
 * Utility to set a CPU mask bit.
 */
static inline void
hal_cpu_mask_set(
	struct hal_cpu_mask *mask,
	hal_cpu_id_t cpu)
{
	if (cpu < HAL_CPU_MAX)
		mask->bits[cpu / 64U] |= (uint64_t)1 << (cpu % 64U);
}

/*
 * Utility to clear a CPU mask bit.
 */
static inline void
hal_cpu_mask_clear(
	struct hal_cpu_mask *mask,
	hal_cpu_id_t cpu)
{
	if (cpu < HAL_CPU_MAX)
		mask->bits[cpu / 64U] &= ~((uint64_t)1 << (cpu % 64U));
}

/*
 * Utility to check a CPU mask bit.
 */
static inline int
hal_cpu_mask_test(
	const struct hal_cpu_mask *mask,
	hal_cpu_id_t cpu)
{
	return cpu < HAL_CPU_MAX &&
	       (mask->bits[cpu / 64U] & ((uint64_t)1 << (cpu % 64U))) != 0;
}


/*
 * IRQ
 */

#define HAL_IRQ_ACK_NONE	((hal_irq_ack_t)0)

/*
 * IRQ acknowledge number.
 */
typedef uintptr_t hal_irq_ack_t;

/*
 * IRQ handler.
 */
typedef void (*hal_irq_handler_t)(int irq, hal_irq_ack_t acknowledge, void *argument);

/*
 * Disable IRQ interrupts. Returns true if currently enabled.
 */
bool
hal_irq_disable(void);

/*
 * Enable IRQ interrupts.
 */
void
hal_irq_enable(void);

/*
 * Set IRQ affinity.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested);

/*
 * Get IRQ affinity.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective);

/*
 * Set an IRQ mask.
 */
void
hal_irq_mask(
	int irq_num);

/*
 * Clear an IRQ mask.
 */
void
hal_irq_unmask(
	int irq_num);

/*
 * Register a handler for a numbered IRQ.
 */
int
hal_irq_register(
	int irq_num,
	hal_irq_handler_t func,
	void *arg);

/*
 * Unregister a handler for a numberd IRQ.
 */
int
hal_irq_unregister(
	int irq_num,
	hal_irq_handler_t func,
	void *arg);

/*
 * Register a handler for a message-signalled logical IRQ.
 *
 * "source" is a canonical bus identity (initially "PCI SSSS:BB:DD.F").
 */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event);

/*
 * Unregister a handler for a message-signalled logical IRQ.
 */
int
hal_irq_unregister_msi(
	int mapped_irq);

/*
 * Send EOI to the IRQ controller.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge);


/*
 * RTC
 *
 * Do not consider timers other than local scheduling ticks.
 */

#define HAL_TIMER_FREQUENCY	(100U)

/*
 * Read wall-clock time as whole seconds since the Unix epoch.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds);

/*
 * Read a fixed-frequency monotonic counter.
 *
 * Its epoch is unspecified and only differences between samples are
 * meaningful.  On false, neither output is changed.  On true, the
 * frequency is nonzero and stable for the boot, and successful
 * operations are linearizable: a later operation never returns a
 * counter below an earlier successful operation, including across
 * CPUs.
 */
bool
hal_rtc_read_counter(
	uint64_t *counter,
	uint64_t *freq_hz);


/*
 * Physical RAM Allocation
 *
 *  - In some architectures such as x86, the physical RAM is divided
 *    into some regions, for example, <640KB, <1MB, 15-16MB hole, and
 *    above 16MB.
 *  - HAL manages the RAM regions. hal_pmem_alloc() just allocates a
 *    block, and it doesn't map the region to a virtual address.
 */

/*
 * Get the total RAM size.
 */
size_t
hal_pmem_get_total_size(void);

/*
 * Allocate a physical memory block.
 */
int
hal_pmem_alloc(
	size_t req_size,
	size_t req_align,
	hal_physaddr_t *block);

/*
 * Translate a physical RAM address to its kernel address.
 *
 * RAM is direct-mapped into the kernel half of the address space, so
 * this is a pure address translation and never fails for managed RAM.
 * Device memory is not direct-mapped; map it with hal_space_map().
 * Returns NULL for an address outside managed RAM.
 */
void *
hal_pmem_to_kernel(
	hal_physaddr_t paddr);

/*
 * Allocate a physical memory block a device can reach.
 *
 * max_paddr is the highest physical address the device can address.
 * boundary, when not zero, is a power-of-two block size the returned
 * range must not cross, for engines whose transfer counter does not
 * carry into the upper address bits.
 */
int
hal_pmem_alloc_limited(
	size_t req_size,
	size_t req_align,
	hal_physaddr_t max_paddr,
	size_t boundary,
	hal_physaddr_t *block);

/*
 * Free a physical memory block. size must be the requested size of the
 * matching allocation; a smaller size splits the block.
 */
int
hal_pmem_free(
	hal_physaddr_t *block,
	size_t size);


/*
 * Space
 *
 *  - In our design, kernels cannot access to page tables directly.
 *  - "Space" abstracts physical to virtual memory mapping.
 *  - A space is for a user process memory space.
 *  - There is the sole kernel space. (address where the MSB is set)
 */

/*
 * Shared system-address selector.  Every user space created by the HAL
 * contains the same architecture-defined system half; this value names that
 * common mapping for system-space operations and is not a detachable task
 * address space.
 */
#define HAL_SPACE_SYS	(NULL)

/*
 * Address space handle.
 */
typedef void *hal_space_t;

/*
 * Page attributes.
 */
#define HAL_SPACE_NONE			(0)
#define HAL_SPACE_READ			(1)
#define HAL_SPACE_WRITE			(2)
#define HAL_SPACE_EXEC			(4)
#define HAL_SPACE_NOCACHE		(8)
#define HAL_SPACE_WRITETHRU		(16)
#define HAL_SPACE_DEVICE		(32)

#define HAL_SPACE_PAGE_PRESENT		0x01U
#define HAL_SPACE_PAGE_ACCESSED		0x02U
#define HAL_SPACE_PAGE_DIRTY		0x04U

/*
 * Create a user space containing the shared system mapping.
 */
hal_space_t
hal_space_create(void);

/*
 * Destroy a user space.  The generic kernel must first retire every owning
 * task and ensure that no CPU selects the space.  HAL implementations close
 * hardware translation windows, but never change task ownership implicitly.
 */
void
hal_space_destroy(
	hal_space_t space);

/*
 * Select a user space on the current CPU; HAL_SPACE_SYS selects only system.
 */
void
hal_space_switch(
	hal_space_t space);

/*
 * Map an address and complete any required TLB synchronization.
 */
int
hal_space_map(
	hal_space_t space,
	void *vaddr,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr);

/*
 * Unmap an address and complete any required TLB synchronization.
 */
int
hal_space_unmap(
	hal_space_t space,
	void *vaddr,
	size_t size);

/*
 * Map a device physical range into kernel space.
 *
 * RAM is direct-mapped and needs no call; device memory is not. The
 * HAL owns the kernel window used for device mappings, so the caller
 * receives the address the HAL chose. attr is the OR of HAL_SPACE_*.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr);

/*
 * Remove a mapping made by hal_space_map_device().
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size);

/*
 * Change protection and complete any required TLB synchronization.
 */
int
hal_space_prot(
	hal_space_t space,
	void *vaddr,
	size_t size,
	uint32_t attr);

/*
 * XXX: Add a single-line explanation here. Should be renamed??
 * XXX: Should be renamed to  hal_space_prot_with_query_flags() ??
 *
 * Atomically publish a protection change, complete every required remote TLB
 * invalidation, and then report the access/dirty state accumulated by the old
 * translations.  flags is the OR of HAL_PAGE_* for the complete range.  In
 * particular, removing HAL_SPACE_WRITE observes stores made through stale TLB
 * entries before the shootdown acknowledgement.  Callers may therefore use
 * this operation as the write-revoke boundary before page writeback.
 */
int
hal_space_prot_query(
	hal_space_t space,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags);

/*
 * XXX: Add a single-line explanation here.
 * XXX: Should be renamed to  hal_space_query_flags() ??
 */
int
hal_space_query(
	hal_space_t space,
	void *vaddr,
	uint32_t *flags);

/*
 * XXX: Add a single-line explanation here. Should be renamed??
 */
int
hal_space_clear_flags(
	hal_space_t space,
	void *vaddr,
	uint32_t flags);

/*
 * Flush TLBs.
 *
 * - If the kernel space is specified by space == NULL, do TLB
 *   shootdown and flush corresponding TLBs on all processors.
 * - If a user space is specified, do TLB shootdown and user space
 *   TLBs will be flushed on all processors where currently selecting
 *   the specified user space.
 */
void
hal_space_flush_tlb(
	hal_space_t space);

/*
 * Flush TLBs only for the spcified range.
 */
void
hal_space_flush_tlb_range(
	hal_space_t space,
	void *vaddr,
	size_t size);

/*
 * Get the page size.
 *
 *  - level > 1 means a large page size.
 */
size_t
hal_space_get_page_size(
	int level);

/*
 * Get the rage of user space virtual address.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit);


/*
 * Task
 *
 * In our HAL design, kernels cannot access to CPU contexts
 * directly. HAL provides abstracted operations on CPU contexts as
 * "tasks". Kernels have to implement processes, threads, and
 * scheduling using "tasks" and "spaces".
 */

/*
 * Task handle.
 */
typedef void *hal_task_t;

/*
 * Wrap the calling CPU's current context as that CPU's initial task.
 * Returns the new task, or NULL when the task record cannot be
 * allocated.  Every CPU calls this once, the boot CPU from the kernel
 * and each secondary CPU from its own HAL bring-up.
 */
hal_task_t
hal_task_create_for_init_context(void);

/*
 * Create a task.
 */
hal_task_t
hal_task_create(
	hal_space_t space,
	void (*start)(void *p),
	void *arg,
	void *user_stack_pointer);

/*
 * Destroy a task.
 */
void
hal_task_destroy(
	hal_task_t t);

/*
 * Switch to a task.
 */
void
hal_task_context_switch(
	hal_task_t t);

/*
 * XXX: Add a single line, easy to understand explanation.
 *
 * Duplicate/replace the active return-to-user context.  These
 * operations are valid only while the current task is handling a user
 * system call.
 */
hal_task_t
hal_task_fork_current(
	hal_space_t child_space,
	intptr_t child_syscall_result);

/*
 * XXX: Add a single line, easy to understand explanation.
 */
int
hal_task_exec_current(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer);

/*
 * XXX: Add a single line, easy to understand explanation.
 */
int
hal_task_exec_validate(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer);

/*
 * Return the active return-to-user stack pointer.
 */
uintptr_t
hal_task_get_user_stack(void);

/*
 * Read the active return-to-user program counter, stack pointer and
 * return value.
 */
int
hal_task_get_user_context(
	uintptr_t *pc,
	uintptr_t *stack_pointer,
	intptr_t *return_value);

/*
 * XXX: Add explanation.
 */
int
hal_task_signal_enter(
	uintptr_t handler,
	uintptr_t stack,
	int signo,
	uintptr_t siginfo,
	uintptr_t ucontext,
	uintptr_t restorer,
	uint32_t token);

/*
 * XXX: Add explanation.
 */
int
hal_task_signal_return(
	uint32_t token,
	intptr_t *return_value);

/*
 * Get the current task.
 */
hal_task_t
hal_task_get_current(void);

/*
 * Set user TLS.
 */
void
hal_task_set_tls(
	hal_task_t t,
	uintptr_t value);

/*
 * Get user TLS.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t t);

/*
 * Opaque kernel ownership link.  HAL stores but never dereferences it.
 */
void
hal_task_set_private(
	hal_task_t t,
	void *private_data);

void *
hal_task_get_private(
	hal_task_t t);

hal_space_t
hal_task_get_space(
	hal_task_t t);

int
hal_task_transfer(
	hal_task_t task,
	hal_cpu_id_t target_cpu);


/*
 * Synchronization
 */

#define hal_compiler_barrier()	__asm__ volatile("" ::: "memory")

/*
 * Memory barrier.
 */
void
hal_mb(void);

void
hal_rmb(void);

void
hal_wmb(void);

void
hal_io_mb(void);

void
hal_io_rmb(void);

void
hal_io_wmb(void);

/*
 * Cache flush
 */
void
hal_icache_invalidate_range(
	uintptr_t addr,
	size_t size);

void
hal_dcache_clean_range(
	uintptr_t addr,
	size_t size);

void
hal_dcache_invalidate_range(
	uintptr_t addr,
	size_t size);

void
hal_dcache_clean_invalidate_range(
	uintptr_t addr,
	size_t size);

void
hal_sync_instruction_stream(
	void *addr,
	size_t size);


/*
 * I/O
 */

uint8_t
hal_io_inp8(
	uint16_t port);

uint16_t
hal_io_inp16(
	uint16_t port);

uint32_t
hal_io_inp32(
	uint16_t port);

void
hal_io_outp8(
	uint16_t port,
	uint8_t value);

void
hal_io_outp16(
	uint16_t port,
	uint16_t value);

void
hal_io_outp32(
	uint16_t port,
	uint32_t value);

uint8_t
hal_mmio_read8(
	const volatile void *addr);

uint16_t
hal_mmio_read16(
	const volatile void *addr);

uint32_t
hal_mmio_read32(
	const volatile void *addr);

uint64_t
hal_mmio_read64(
	const volatile void *addr);

void
hal_mmio_write8(
	volatile void *addr,
	uint8_t value);

void
hal_mmio_write16(
	volatile void *addr,
	uint16_t value);

void
hal_mmio_write32(
	volatile void *addr,
	uint32_t value);

void
hal_mmio_write64(
	volatile void *addr,
	uint64_t value);


/*
 * Misc
 */

/*
 * Return an architecture-specific boot handoff object by name.  The
 * returned object remains owned by HAL.  Unknown or unavailable
 * handoffs return NULL.
 */
void *
hal_get_arch_handoff(
	const char *name);

/*
 * Do system reset for a reboot.
 */
void
hal_reset(void);

/*
 * Do system power off.
 */
void
hal_poweroff(void);

/*
 * Halt for a kernel panic.
 */
void
hal_panic(void);


/*
 * Fill a buffer from a platform cryptographic entropy source. Ports which
 * have no such source return false. Buffer contents are unspecified when the
 * function returns false.
 */
bool
hal_entropy_fill(
	void *buffer,
	size_t size);


/*
 * Memory Usage 
 */

struct hal_memstat {
	size_t physical_total;
	size_t physical_reserved;
	size_t physical_allocated;
	size_t physical_free;

	size_t task_stack_bytes;
	uint32_t task_count;

	uint32_t space_count;
	uint32_t page_table_count;

	/* Optional boot-range observations. Zero validity means unavailable. */
	uint32_t boot_ranges_valid;
	uint32_t boot_range_count;
	uint64_t boot_usable_bytes;
	uint64_t boot_highest_end;
	uint64_t boot_usable_highest_end;
	uint64_t boot_reclaim_bytes;
	uint32_t boot_memory_source;

	uint64_t direct_mapped_bytes;

	uint64_t allocator_initial_bytes;
	uint64_t allocator_metadata_bytes;
	uint64_t allocator_scan_words;
	uint64_t allocator_max_extent_scan_words;
	uint64_t allocator_max_irqoff_cycles;
};

/*
 * Get the memory usage statistics.
 */
void
hal_get_memstat(
	struct hal_memstat *stat);


/*
 * Kernel-side Entrypoints
 */

/*
 * Syscall arguments.
 */
#define HAL_SYSCALL_ARGS		(6)

/*
 * Signal nest.
 */
#define HAL_SIGNAL_NEST_MAX		(8)

/*
 * Trap cause.
 */
enum hal_trap_cause {
	HAL_TRAP_CAUSE_PAGE_FAULT,
	HAL_TRAP_CAUSE_ILLEGAL_INSN,
	HAL_TRAP_CAUSE_BREAKPOINT,
	HAL_TRAP_CAUSE_ALIGNMENT,
	HAL_TRAP_CAUSE_MACHINE_CHECK,
	HAL_TRAP_CAUSE_ARITHMETIC,
	HAL_TRAP_CAUSE_PROTECTION,
	HAL_TRAP_CAUSE_OTHER
};

/*
 * Trap mode.
 */
enum hal_trap_mode {
	HAL_TRAP_MODE_READ,
	HAL_TRAP_MODE_WRITE,
	HAL_TRAP_MODE_EXEC,
	HAL_TRAP_MODE_NONE
};

/*
 * Trap return.
 */
enum hal_trap_ret {
	HAL_TRAP_RET_SUCCESS,
	HAL_TRAP_RET_FAILED
};

/*
 * Entrypoint for the primary CPU.
 */
void
kernel_entry(
	const void *handoff);

/*
 * Entrypoint for the secondary CPUs.
 */
void
kernel_secondary_entry(
	hal_cpu_id_t cpu);

/*
 * Yield the current task while keeping it runnable.
 *
 * - This function must not be called from the kernel.
 */
void
kernel_yield_task(void);

/*
 * Do "wait" behavior of the wait/notify protocol.
 *
 * - This function must not be called from the kernel.
 */
void
kernel_wait_task(void);

/*
 * Do "notify" behavior of the wait/notify protocol for other tasks.
 *
 * - This function must not be called from the kernel.
 */
void
kernel_notify_task(
	hal_task_t task);

/*
 * Scheduling timer callback.
 */
void
kernel_timer_handler(
	hal_cpu_id_t cpu,
	hal_irq_ack_t acknowledge);

/*
 * IPI handler callback.
 */
void
kernel_cpu_notify_handler(
	hal_cpu_id_t cpu,
	hal_irq_ack_t acknowledge);

/*
 * System call entry.
 *
 * The HAL installs the active return-to-user frame and calls this
 * with local IRQs masked.  The generic kernel owns any interruptible
 * syscall window and returns with local IRQs masked so the HAL can
 * commit the saved frame atomically.
 */
intptr_t
kernel_syscall_handler(
	uint32_t number,
	const uintptr_t args[HAL_SYSCALL_ARGS]);

/*
 * User fault entry.
 *
 * A user frame is active and local IRQs are masked on entry and
 * normal return.  The generic kernel owns fault accounting and any
 * interruptible fault-resolution window.
 */
int
kernel_user_fault_handler(
	enum hal_trap_cause cause,
	enum hal_trap_mode mode,
	uintptr_t pc,
	uintptr_t address,
	uintptr_t vector,
	uintptr_t error_code);

/*
 * System fault entry.
 *
 * Supervisor fault entry.  No user frame is published and the HAL
 * keeps its own saved frame.  SUCCESS resumes the interrupted kernel
 * code. FAILED leaves the register diagnostics and the stop to the
 * HAL.
 */
int
kernel_sys_fault_handler(
	enum hal_trap_cause cause,
	enum hal_trap_mode mode,
	uintptr_t pc,
	uintptr_t address,
	uintptr_t vector,
	uintptr_t error_code);

/*
 * Called once on the way back to user mode, after a system call, a
 * fault or an interrupt has been handled.
 *
 * This is the kernel's last chance to act on the returning task, so it
 * applies pending signals, stop requests and exit here. The user frame
 * is still attached and may be changed. Local IRQs are masked on entry
 * and must be masked again on return, but the kernel may enable them
 * while it works. Any interrupt source has already been quiesced and
 * acknowledged by the HAL.
 */
void
kernel_user_return_handler(void);

/*
 * Kernel console output, or NULL until the kernel console exists.
 *
 * The kernel publishes this with a RELEASE store once its console can
 * draw. hal_putc() reads it with ACQUIRE and delegates when it is set.
 * It is called from interrupt context and from panic, so it must not
 * sleep and must take only IRQ-safe locks.
 */
extern void (*kernel_putc)(int c);

/*
 * Allocator. Called only after the invocation of kernel_entry().
 */
void *kernel_alloc(size_t size);

/*
 * Deallocator.
 */
void kernel_free(void *p);

#endif
