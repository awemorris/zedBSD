/*
 * Kernel HAL
 * Copyright (C) 2026, Awe Morris.
 *
 * This header defines a kernel porting HAL. A HAL is implemented for
 * a combination of a CPU architecture and a machine/board type. A HAL
 * doesn't implement basic kernel features such as scheduling
 * algorithm, and only implements low level operations required for
 * contemporary 32-bit and 64-bit POSIX-compatible kernels.
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef SYS_KERN_HAL_H
#define SYS_KERN_HAL_H

#include <hal/types.h>

struct hal_cpu_mask;

/*
 * Kernel C runtime
 */

#define HAL_ASSERT(e) ((e) ? (void)0 : hal_assert(__FILE__, __LINE__, #e))
#define HAL_FATAL(msg) hal_fatal(__FILE__, __LINE__, msg)

int hal_strlen(const char *s);
void *hal_memset(void *s, int c, size_t n);
void *hal_memset16(uint16 *s, uint16 c, size_t n);
void *hal_memset32(uint32 *s, uint32 c, size_t n);
void *hal_memcpy(void *dest, const void *src, size_t n);

/* Freestanding compilers may emit calls to these bare names. */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

/* The embedding kernel supplies the allocator used by the HAL. */
void hal_set_allocator(void *(*alloc)(size_t size), void (*free)(void *p));
void *hal_malloc(size_t size);
void hal_free(void *ptr);

int hal_putchar(int c);
int hal_puts(const char *s);
int hal_printf(const char *format, ...);

void hal_assert(const char *file, int line, const char *exp);
void hal_fatal(const char *file, int line, const char *s);

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

/* Local IRQ lock and interrupt-service-task interface. */
typedef int irqlock_t;

#define ENTER_IRQLOCK(v) \
	do { \
		(v) = irq_acquire_lock(); \
	} while (0);

#define LEAVE_IRQLOCK(v) \
	do { \
		irq_unacquire_lock(v); \
	} while (0)

irqlock_t irq_acquire_lock(void);
void irq_unacquire_lock(irqlock_t lock);
void irq_enter_isr(int irq_num);
void irq_leave_isr(int irq_num);

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

/* Legacy BSP tick accessor. */
hal_clock_t clock_get_tick_count(void);

/*
 * System Call
 */

#define HAL_SYSCALL_ARGS	(6)

typedef intptr_t (*hal_syscall_handler_t)(uint32_t number,
					  const uintptr_t args[HAL_SYSCALL_ARGS]);
void hal_syscall_set_handler(hal_syscall_handler_t handler);

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

#define HAL_PAGE_PRESENT  0x01U
#define HAL_PAGE_ACCESSED 0x02U
#define HAL_PAGE_DIRTY    0x04U
int hal_page_query(hal_space_t space, void *vaddr, uint32_t *flags);
int hal_page_clear_flags(hal_space_t space, void *vaddr, uint32_t flags);

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
void hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit);

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

struct hal_memory_stats {
	size_t physical_total;
	size_t physical_reserved;
	size_t physical_allocated;
	size_t physical_free;
	size_t task_stack_bytes;
	uint32_t task_count;
	uint32_t space_count;
	uint32_t page_table_count;
};

void hal_memory_get_stats(struct hal_memory_stats *);

/* Low-level physical-memory descriptor used inside the HAL. */
struct pmem_desc {
	void *vaddr;
	void *paddr;
	size_t size;
};

#define PMEM_SUCCESS (0)
#define PMEM_NOSPACE (1)
#define PMEM_BADDESC (2)

int pmem_alloc_lo(size_t size, struct pmem_desc *desc);
int pmem_free(struct pmem_desc *desc);
void pmem_reserve(hal_physaddr_t paddr, size_t size);

/* PC-98 memory-controller and BIOS-work-area services. */
void hal_pc98_enable_high_memory(void);
void hal_pc98_memory_segments(uint32_t *low_extended, uint32_t *high_mib);


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

/* Wrap the CPU context which entered kernel_entry() as the initial task. */
void hal_task_init(void);

/* Create a task. */
hal_task_t hal_task_create(hal_space_t space, void (*start)(void *p), void *arg, void *user_stack_pointer);

/* Duplicate/replace the active return-to-user context.  These operations are
 * valid only while the current task is handling a user system call. */
hal_task_t hal_task_fork_current(hal_space_t child_space,
				 intptr_t child_syscall_result);
int hal_task_exec_current(hal_space_t new_space, uintptr_t entry,
			  uintptr_t user_stack_pointer);
uintptr_t hal_task_user_stack(void);
int hal_task_signal_enter(uintptr_t handler, uintptr_t stack, int signo,
			  uintptr_t restorer, uint32_t token);
int hal_task_signal_return(uint32_t token, intptr_t *return_value);

typedef void (*hal_user_return_handler_t)(void);
void hal_user_return_set_handler(hal_user_return_handler_t handler);
void hal_user_return_invoke(void);

/* Destroy a task. */
void hal_task_destroy(hal_task_t t);

/* Switch to a task. */
void hal_task_context_switch(hal_task_t t);

/* Atomically enable interrupts and halt, then return with IRQs disabled. */
void hal_cpu_idle(void);

/* Get the current task. */
hal_task_t hal_task_get_current(void);

/* Set user TLS. */
void hal_task_set_tls(hal_task_t t, uintptr_t value);

/* Get user TLS. */
uintptr_t hal_task_get_tls(hal_task_t t);

/* Opaque kernel ownership link.  HAL stores but never dereferences it. */
void hal_task_set_private(hal_task_t t, void *private_data);
void *hal_task_get_private(hal_task_t t);
hal_space_t hal_task_get_space(hal_task_t t);

/* Temporary ring-3 trap observation hooks used until the syscall ABI exists. */
struct hal_user_trap {
	uint32_t vector;
	uint32_t cs;
#if UINTPTR_MAX == UINT64_MAX
	uint64_t eip;
	uint64_t eax;
	uint64_t error_code;
	uint64_t fault_address;
#elif UINTPTR_MAX == UINT32_MAX
	uint32_t eip;
	uint32_t eax;
	uint32_t error_code;
	uint32_t fault_address;
#else
#error "Unsupported pointer size for struct hal_user_trap"
#endif
};
typedef void (*hal_user_int_handler_t)(const struct hal_user_trap *);
typedef int (*hal_user_fault_handler_t)(const struct hal_user_trap *);
void hal_user_int_set_handler(hal_user_int_handler_t handler);
void hal_user_fault_set_handler(hal_user_fault_handler_t handler);
void hal_reschedule_on_interrupt_return(void);

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

enum hal_cons_mode {
	HAL_CONS_FIXED_MENU,
	HAL_CONS_TERMINAL,
};

#define HAL_CONS_COLUMNS 80U
#define HAL_CONS_ROWS 25U
#define HAL_CONS_NORMAL_ATTRIBUTE 0xe1U

enum hal_key {
	HAL_KEY_ESCAPE = 0x1b,
	HAL_KEY_BACKSPACE = 0x08,
	HAL_KEY_TAB = 0x09,
	HAL_KEY_ENTER = 0x0d,
	HAL_KEY_PAGE_UP = 0x136,
	HAL_KEY_PAGE_DOWN = 0x137,
	HAL_KEY_INSERT = 0x138,
	HAL_KEY_DELETE = 0x139,
	HAL_KEY_UP = 0x13a,
	HAL_KEY_LEFT = 0x13b,
	HAL_KEY_RIGHT = 0x13c,
	HAL_KEY_DOWN = 0x13d,
	HAL_KEY_HOME = 0x13e,
	HAL_KEY_END = 0x13f,
	HAL_KEY_F1 = 0x162,
	HAL_KEY_F2 = 0x163,
	HAL_KEY_F3 = 0x164,
	HAL_KEY_F4 = 0x165,
	HAL_KEY_F5 = 0x166,
	HAL_KEY_F6 = 0x167,
	HAL_KEY_F7 = 0x168,
	HAL_KEY_F8 = 0x169,
	HAL_KEY_F9 = 0x16a,
	HAL_KEY_F10 = 0x16b,
	HAL_KEY_SHIFT = 0x170,
};

#define HAL_KEY_EVENT_KEY_MASK 0x000001ffU
#define HAL_KEY_EVENT_SHIFT    0x00010000U
#define HAL_KEY_EVENT_CTRL     0x00020000U
#define HAL_KEY_EVENT_GRAPH    0x00040000U

struct hal_cons_state {
	enum hal_cons_mode mode;
	unsigned row;
	unsigned column;
	int cursor_visible;
};

void bsp_cons_init(void);
void cons_cls(void);
void cons_putc(int c);
void cons_puts(const char *utf8);
int cons_getc(void);
void cons_set_attr(int fg, int bg);

void hal_cons_reset(void);

/* Put a character on the kernel console. */
void hal_cons_putc(int c);

/* Clear the console. */
void hal_cons_clear(void);

/* Move cursor. */
void hal_cons_move_cursor(int line, int col);

/* Get a character on the kernel console. */
int hal_cons_getc(void);

void hal_cons_set_mode(enum hal_cons_mode mode);
void hal_cons_write(const char *utf8);
void hal_cons_write_n(const char *utf8, unsigned length);
void hal_cons_write_at(unsigned row, unsigned column, const char *utf8);
void hal_cons_clear_row(unsigned row);
void hal_cons_clear_to_eol(void);
int hal_cons_write_at_attr(unsigned row, unsigned column, const char *utf8,
			   uint8_t attribute);
int hal_cons_write_n_at(unsigned row, unsigned column, const char *utf8,
			unsigned length, uint8_t attribute);
int hal_cons_clear_to_eol_at(unsigned row, unsigned column);
int hal_cons_set_cursor(unsigned row, unsigned column);
void hal_cons_show_cursor(int visible);
void hal_cons_save_state(struct hal_cons_state *state);
void hal_cons_restore_terminal(const struct hal_cons_state *state);
void hal_cons_update_cursor(void);
int hal_cons_read_event(void);
int hal_cons_poll_event(void);
int hal_cons_key_state(int key);
void hal_cons_drain_input(void);
unsigned hal_cons_modifiers(void);

/*
 * Framebuffer ownership
 */

void fb_set_active(int active);
int fb_is_active(void);

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
void kernel_entry(const void *handoff);

/* Interval timer handler. */
void kernel_timer_handler(void);

#endif
