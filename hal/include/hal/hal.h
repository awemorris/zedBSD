/*
 * Kernel HAL
 *
 * This header defines a kernel porting HAL. A HAL is implemented for
 * a combination of a CPU architecture and a machine/board type. A HAL
 * doesn't implement basic kernel features such as scheduling
 * algorithm, and only implements low level operations required for
 * contemporary 32-bit and 64-bit POSIX-compatible kernels.
 */

#ifndef SYS_KERN_HAL_H
#define SYS_KERN_HAL_H

#include <sys/types.h>

/*
 * SMP
 *
 * Secondary processors are initialized inside HAL, and waiting for
 * IPI at the moment kernel_main() called.
 */

#define HAL_CORE_MAX		(512)

/* Get the current CPU index. */
int hal_get_current_cpu(void);

/* Get the number of the CPUs. */
int hal_get_cpu_count(void);

/* Send IPI. */
void hal_send_ipi_one(int cpu, int ipi_num);

/* Send IPI. */
void hal_send_ipi_mask(uint8_t *mask, int ipi_num);

/* Send IPI. */
void hal_send_ipi_others(int ipi_num);

/*
 * IRQ
 */

/* Disable IRQ interrupts. Returns true if currently enabled. */
bool hal_irq_disable(void);

/* Enable IRQ interrupts. */
void hal_irq_enable(void);

/* Set IRQ CPU affinity. */
void hal_irq_set_affinity(int irq, struct hal_cpu_mask cpu_mask);

/* Set an IRQ mask. */
void hal_irq_mask(int irq_num);

/* Clear an IRQ mask. */
void hal_irq_unmask(int irq_num);

/* Send EOI to the IRQ controller. */
void hal_irq_send_eoi(int irq);

/* Set an IRQ handler. */
void hal_irq_set_handler(int irq_num, void (*func)(void *arg), void *arg);

/*
 * Interval Timer
 *
 * Do not consider timers other than local scheduling ticks.
 */

/* Set the timer frequency. */
void hal_timer_set_freq(uint32_t freq);

/* Get the timer tick. */
uint64_t hal_timer_get_tick(void);

/* Read the RTC. */
uint64_t hal_timer_read_rtc(void);

/*
 * System Call
 */

#define HAL_SYSCALL_ARGS	(7)

void hal_syscall_set_handler(void *(*handler)(int num, void *arg[]));

/*
 * Trap Handlers
 */

struct hal_reg_set;

#define HAL_TRAP_CAUSE_PAGE_FAULT	(0)
#define HAL_TRAP_CAUSE_ILLEGAL_INSN	(1)
#define HAL_TRAP_CAUSE_BREAKPOINT	(2)
#define HAL_TRAP_CAUSE_ALIGNMENT	(3)
#define HAL_TRAP_CAUSE_MACHINE_CHECK	(4)

#define HAL_TRAP_MODE_READ		(0)
#define HAL_TRAP_MODE_WRITE		(1)
#define HAL_TRAP_MODE_EXEC		(2)

#define HAL_TRAP_RET_SUCCESS		(0)
#define HAL_TRAP_RET_FAILED		(1)

typedef int (*hal_trap_handler_t)(void *pc, void *addr, int mode);

void hal_set_trap_handler(int trap, hal_trap_handler_t handler);

/*
 * Memory
 *
 * "Space" is an abstracted page table. In our design, kernels cannot
 * access to page tables directly.
 */

/* Address space handle. */
typedef void *hal_space_t;

/* System space. */
#define HAL_SPACE_SYS		(NULL)

/* Page attributes. */
#define HAL_SPACE_NONE		(0)
#define HAL_SPACE_READ		(1)
#define HAL_SPACE_WRITE		(2)
#define HAL_SPACE_EXEC		(4)
#define HAL_SPACE_NOCACHE	(8)
#define HAL_SPACE_WRITETHRU	(16)
#define HAL_SPACE_DEVICE	(32)

/* Memory map entry attributes. */
#define HAL_PAGE_ENTRY_NONE	(0)	/* Memory not implemented. */
#define HAL_PAGE_ENTRY_RAM	(1)	/* RAM implemented. */
#define HAL_PAGE_ENTRY_ROM	(2)	/* ROM implemented. */
#define HAL_PAGE_ENTRY_DEVICE	(4)	/* Device memory implemented. */
#define HAL_PAGE_ENTRY_KERNEL	(8)	/* Kernel loaded. */
#define HAL_PAGE_ENTRY_SPECIAL	(16)	/* Special. (e.g., interrupt vector) */
#define HAL_PAGE_ENTRY_MMIO	(32)	/* MMIO region. */

/* Memory map entry. */
struct hal_memory_map_entry {
	uintptr_t base;
	uintptr_t size;
	uint32_t flags;
};

/* Get the memory map. */
void hal_mem_get_memory_map(int *blocks, struct hal_memory_map_entry *entries, size_t buf_count);

/* Create a user space. */
hal_space_t hal_mem_create_space(void);

/* Destroy a user space. */
void hal_page_destroy_space(hal_space_t space);

/* Switch the user space of the current task. */
void hal_page_switch_space(hal_space_t space);

/* Map address. */
int hal_page_map(hal_space_t space, void *vaddr, uintptr_t paddr, size_t size, uint32_t attr);

/* Map address. Additional TLB flush is required if the current space is specified. */
int hal_page_prot(hal_space_t space, void *vaddr, size_t size, uint32_t attr);

/* Unmap address. Additional TLB flush is required if the current space is specified. */
int hal_page_unmap(hal_space_t space, void *vaddr, size_t size);

/*
 * Flush TBLs.
 *
 * - If the kernel space is specified by space == NULL, do TLB
 *   shootdown and flush corresponding TLBs on all processors.
 * - If a user space is specified, do TLB shootdown and user space
 *   TLBs will be flushed on all processors where currently selecting
 *   the specified user space.
 */
void hal_page_flush_tlb(hal_space_t space);

/* Get the page size. (level > 1 means a large page size.) */
size_t hal_page_get_page_size(int level);

/*
 * Physical Memory Allocation (Page Unit)
 */

/* Error Code */
#define HAL_PMEM_SUCCESS	(0)
#define HAL_PMEM_NOSPACE	(1)
#define HAL_PMEM_BADDESC	(2)

/* Flags. */
#define HAL_PMEM_ATTR_NOCACHE		(1)
#define HAL_PMEM_ATTR_WRITETHRU		(2)

/* Memory Block Descriptor */
struct hal_pmem {
	uintptr_t vaddr;
	uintptr_t paddr;
	size_t size;
};

/* Allocate a physical memory block. */
int hal_pmem_alloc(size_t size, struct hal_pmem *desc, uint32_t flags);

/* Allocate a physical memory block. (with range limit) */
int hal_pmem_alloc_limited(size_t size, uintptr_t above, uintptr_t below, struct hal_pmem *desc);

/* Free a physical memory block. */
int hal_pmem_free(struct hal_pmem *desc);

/* Get the total RAM size. */
size_t hal_pmem_get_total_size(void);


/*
 * Task
 *
 * In our HAL design, kernels cannot access to CPU contexts
 * directly. HAL provides abstracted operations on CPU contexts as
 * "tasks". Kernels have to implement processes, threads, and
 * scheduling using "tasks" and "spaces".
 */

/* Task handle. */
typedef void *hal_task_t;

/* Create a task. */
hal_task_t hal_task_create(hal_space_t space, void (*start)(void *p), void *arg, void *user_stack_pointer);

/* Destroy a task. */
void hal_task_destroy(hal_task_t t);

/* Switch to a task. */
void hal_task_context_switch(hal_task_t t);

/* Get the current task. */
hal_task_t hal_task_get_current(void);

/* Set user TLS. */
void hal_task_set_tls(hal_task_t t, uintptr_t value);

/* Get user TLS. */
uintptr_t hal_task_get_tls(hal_task_t t);

/*
 * Synchronization
 */

#define hal_compiler_barrier()	__asm__ volatile("" ::: "memory")

/* Memory barrier. */
void hal_mb(void);
void hal_rmb(void);
void hal_wmb(void);
void hal_io_mb(void);
void hal_io_rmb(void);
void hal_io_wmb(void);

/* Cache flush */
void hal_icache_invalidate_range(uintptr_t addr, size_t size);
void hal_dcache_clean_range(uintptr_t addr, size_t size);
void hal_dcache_invalidate_range(uintptr_t addr, size_t size);
void hal_dcache_clean_invalidate_range(uintptr_t addr, size_t size);
void hal_sync_instruction_stream(void *addr, size_t size);

/* Halt until a next interrupt. */
void hal_halt(void);

/*
 * I/O
 */

uint8_t hal_io_inp8(uint16_t port);
uint16_t hal_io_inp16(uint16_t port);
uint32_t hal_io_inp32(uint16_t port);

void hal_io_outp8(uint16_t port, uint8_t value);
void hal_io_outp16(uint16_t port, uint16_t value);
void hal_io_outp32(uint16_t port, uint32_t value);

uint8_t hal_mmio_read8(const volatile void *addr);
uint16_t hal_mmio_read16(const volatile void *addr);
uint32_t hal_mmio_read32(const volatile void *addr);
uint64_t hal_mmio_read64(const volatile void *addr);

void hal_mmio_write8(volatile void *addr, uint8_t value);
void hal_mmio_write16(volatile void *addr, uint16_t value);
void hal_mmio_write32(volatile void *addr, uint32_t value);
void hal_mmio_write64(volatile void *addr, uint64_t value);

/*
 * Console
 */

/* Put a character on the kernel console. */
void hal_cons_putc(int c);

/* Clear the console. */
void hal_cons_clear(void);

/* Move cursor. */
void hal_cons_move_cursor(int line, int col);

/* Get a character on the kernel console. */
int hal_cons_getc(void);

/*
 * Misc
 */

void hal_reset(void);
void hal_poweroff(void);
void hal_panic(void);

/*
 * Kernel-side Entrypoints
 */

/* Entrypoint. */
void kernel_entry(void);

/* Interval timer handler. */
void kernel_timer_handler(void);

#endif
