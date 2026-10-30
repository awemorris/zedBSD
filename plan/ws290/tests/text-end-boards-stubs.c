/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's services and the boards' other drivers, as stubs, for the
 * host test of the console's leaving of the display on the boards other
 * than the PC/AT (text-end-boards-host-test.c).
 *
 * The VisionFive 2's binding is included here whole, so that the test
 * reaches its statics through the accessors at the end and the stubs of
 * the drivers it calls are checked against their real declarations.  The
 * hardware layer's character path follows its contract: once the kernel
 * published kernel_putc, every character goes there; before, the stub
 * counts it as drawn by the hardware layer.
 */

#include "kern/platform/visionfive2.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/platform/pc98/graphics/display-auto.h"
#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/dcache.h"
#include "kern/device-io.h"
#include "kern/irq.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/tty.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"

/* How many device mappings the stub keeps apart. */
#define STUB_MAPPINGS_MAX	16U

/* The UARTs the test's device tree places, and the register file each maps to. */
#define STUB_UART_CONSOLE	0x10000000ULL
#define STUB_UART_OTHER		0x10010000ULL

/* One device mapping the stub handed out. */
struct stub_mapping {
	uint64_t physical;
	size_t size;
	void *pointer;
};

/* The kernel's character path, which the hardware layer reads. */
void (*kernel_putc)(int c);

/* What the hardware layer's character path drew itself. */
unsigned hal_drawn;

/* The UARTs' registers: a 32-bit register every four bytes. */
uint32_t uart_console_registers[64];
uint32_t uart_other_registers[64];

/* A byte-wide UART's registers, one byte apart. */
uint8_t uart_byte_registers[16];

/* How the line status answers: reads before the transmitter frees, or never. */
unsigned uart_busy_reads;
int uart_never_ready;
unsigned uart_status_reads;

/* What reached the transmitters. */
uint8_t uart_sent[512];
unsigned uart_sent_count;
unsigned uart_other_writes;

/* The device mappings made and removed, and a removal's failure to inject. */
struct stub_mapping mappings[STUB_MAPPINGS_MAX];
unsigned mapping_count;
unsigned unmap_count;
uint64_t unmapped_physical[STUB_MAPPINGS_MAX];
size_t unmapped_size[STUB_MAPPINGS_MAX];
uint64_t unmap_fail_physical;
int unmap_fail_error;

/* The firmware's screen kern_boot_framebuffer() describes, under one name. */
const char *screen_name;
uint64_t screen_physical;
uint64_t screen_size;

/* The interrupt state's changes. */
unsigned irq_disables;
unsigned irq_enables;

/* The Raspberry Pi hardware layer's console calls. */

/* The PC-98 native display's calls, and the device /dev/graphics registered. */
unsigned pc98_enters;
unsigned pc98_fills;
unsigned pc98_leaves;
unsigned io_writes;
const struct cdev_ops *graphics_cdev;

/* The device tree the hardware layer hands over: its address and size. */
uint64_t fdt_range[2];

/* The JIS table the PC-98 graphics driver owns. */
const uint16_t drv_pc98_jisx0208_to_ucs[7896];

static struct stub_mapping *mapping_find(void *pointer);
static int pc98_enter(void *context, struct pc98_display_info *info);
static void pc98_leave(void *context);
static int pc98_fill(void *context, const struct pc98_display_rect *rect, uint32_t color);
static uint32_t uart_read(const volatile void *address);
static void uart_write(volatile void *address, uint32_t value);

/* The VisionFive 2's statics, for the test. */

void
vf2_test_set_fatal_display(
	void (*display)(void))
{
#if CONFIG_DRIVER_POWERVR
	vf2_fatal_display = display;
#else
	(void)display;
#endif
}

int
vf2_test_uart_present(void)
{
	return console_uart_present;
}

uint64_t
vf2_test_uart_physical(void)
{
	return console_uart.physical;
}

/* The hardware layer's character path: the kernel's once published. */
void
hal_putc(
	int c)
{
	void (*kernel_output)(int character);

	/* Delegates after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(c);
		return;
	}

	/* The hardware layer draws it. */
	hal_drawn++;
}

bool
hal_irq_disable(void)
{
	irq_disables++;
	return true;
}

void
hal_irq_enable(void)
{
	irq_enables++;
}

hal_cpu_id_t
hal_cpu_current(void)
{
	return 0;
}

void *
hal_get_arch_handoff(
	const char *name)
{
	/* Only the device tree is handed over. */
	if (strcmp(name, "riscv64.fdt") == 0 && fdt_range[0] != 0U)
		return fdt_range;
	return NULL;
}

void
hal_halt(void)
{
	abort();
}

void
hal_poweroff(void)
{
	abort();
}

void
hal_reset(void)
{
	abort();
}

void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* The test's "physical" addresses are its own pointers. */
	return (void *)(uintptr_t)address;
}

int
kern_boot_framebuffer(
	const char *prefix,
	struct kern_boot_framebuffer *framebuffer)
{
	/* Starts from no screen. */
	memset(framebuffer, 0, sizeof(*framebuffer));
	if (screen_name == NULL || strcmp(prefix, screen_name) != 0)
		return ENOENT;

	/* The screen the scene set. */
	framebuffer->physical_base = screen_physical;
	framebuffer->size = screen_size;
	framebuffer->width = 640U;
	framebuffer->height = 480U;
	framebuffer->pitch = 640U * 4U;
	framebuffer->format = KERN_BOOT_FRAMEBUFFER_RGBX8888;
	return 0;
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	struct stub_mapping *mapping;

	(void)attributes;

	/* A full table refuses. */
	if (mapping_count >= STUB_MAPPINGS_MAX)
		return ENOMEM;
	mapping = &mappings[mapping_count];
	mapping->physical = address;
	mapping->size = size;

	/* The UARTs have register files; anything else gets RAM. */
	if (address == STUB_UART_CONSOLE) {
		mapping->pointer = uart_console_registers;
	} else if (address == STUB_UART_OTHER) {
		mapping->pointer = uart_other_registers;
	} else {
		mapping->pointer = calloc(1U, size);
		if (mapping->pointer == NULL)
			return ENOMEM;
	}

	/* Handed out. */
	mapping_count++;
	*mapped = mapping->pointer;
	return 0;
}

int
kern_device_unmap(
	void *pointer,
	size_t size)
{
	struct stub_mapping *mapping;

	/* A mapping the stub never made is refused. */
	mapping = mapping_find(pointer);
	if (mapping == NULL)
		return EINVAL;

	/* An injected failure keeps the mapping. */
	if (unmap_fail_error != 0 && mapping->physical == unmap_fail_physical)
		return unmap_fail_error;

	/* Removed and recorded. */
	if (unmap_count < STUB_MAPPINGS_MAX) {
		unmapped_physical[unmap_count] = mapping->physical;
		unmapped_size[unmap_count] = size;
	}
	unmap_count++;
	if (mapping->pointer != uart_console_registers && mapping->pointer != uart_other_registers)
		free(mapping->pointer);
	mapping->pointer = NULL;
	return 0;
}

uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	return (uint8_t)uart_read(address);
}

uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	return uart_read(address);
}

void
kern_mmio_write8(
	volatile void *address,
	uint8_t value)
{
	uart_write(address, value);
}

void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uart_write(address, value);
}

uint8_t
kern_io_in8(
	uint16_t port)
{
	(void)port;

	/* The GDC's command FIFO always has room. */
	return 0U;
}

void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	(void)port;
	(void)value;
	io_writes++;
}

int
kern_irq_register(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	(void)irq;
	(void)handler;
	(void)argument;
	return 0;
}

void
kern_irq_unmask(
	int irq)
{
	(void)irq;
}

void
kern_irq_send_eoi(
	kern_irq_ack_t acknowledge)
{
	(void)acknowledge;
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(
	struct spinlock *lock)
{
	lock->held.value = 1U;
}

int
spin_trylock(
	struct spinlock *lock)
{
	/* Taken when free. */
	if (lock->held.value != 0U)
		return 0;
	lock->held.value = 1U;
	return 1;
}

void
spin_unlock(
	struct spinlock *lock)
{
	lock->held.value = 0U;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* A second take on the one thread would never come back. */
	if (lock->held.value != 0U) {
		printf("FAIL %s taken twice\n", lock->name);
		abort();
	}
	lock->held.value = 1U;
	return 0UL;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)enabled;
	lock->held.value = 0U;
}

int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	memset(mutex, 0, sizeof(*mutex));
	spin_init(&mutex->guard, rank, name);
	return 0;
}

void
mutex_lock(
	struct mutex *mutex)
{
	/* One thread: a second take is a bug of the code under test. */
	if (mutex->locked != 0U) {
		printf("FAIL %s locked twice\n", mutex->guard.name);
		abort();
	}
	mutex->locked = 1U;
}

void
mutex_unlock(
	struct mutex *mutex)
{
	mutex->locked = 0U;
}

void
waitq_wake_all(
	struct wait_queue *queue)
{
	(void)queue;
}

int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data)
{
	(void)name;
	(void)rdev;
	(void)data;

	/* The test opens the device through its operations. */
	graphics_cdev = ops;
	return 0;
}

int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size)
{
	memcpy((void *)destination, source, size);
	return 0;
}

void
drv_pc98_auto_default(
	struct pc98_auto *backend,
	pc98_display_reset_fn display_reset,
	pc98_display_reset_fn display_stop,
	void *bios_context,
	pc98_in8_fn port_in8,
	pc98_out8_fn port_out8,
	void *io_context,
	volatile uint8_t *cirrus_framebuffer)
{
	(void)display_reset;
	(void)display_stop;
	(void)bios_context;
	(void)port_in8;
	(void)port_out8;
	(void)io_context;
	(void)cirrus_framebuffer;
	memset(backend, 0, sizeof(*backend));
}

int
drv_pc98_auto_make_hal(
	struct pc98_display_backend *hal,
	struct pc98_auto *backend)
{
	(void)backend;

	/* A native display that counts its calls. */
	memset(hal, 0, sizeof(*hal));
	hal->display.enter = pc98_enter;
	hal->display.leave = pc98_leave;
	hal->display.fill = pc98_fill;
	return 1;
}

int
drv_pc98_glyph_get_bitmap(
	struct pc98_glyph *backend,
	uint32_t codepoint,
	uint8_t font[32],
	unsigned *width,
	unsigned *height)
{
	(void)backend;
	(void)codepoint;
	(void)font;
	(void)width;
	(void)height;
	return 0;
}

int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	(void)entry;
	(void)arg;
	(void)priority;
	(void)result;
	return ENOMEM;
}

void
thread_start(
	struct thread *thread)
{
	(void)thread;
}

void
sched_sleep(
	uint64_t timeout_tick)
{
	(void)timeout_tick;
}

uint64_t
sched_ticks(void)
{
	return 0U;
}

uint64_t
clock_ticks(void)
{
	return 0U;
}

void
tty_console_input_byte(
	uint8_t byte)
{
	(void)byte;
}

/* The early console is the hardware layer's character path. */
void
kern_console_early_putc(
	int character)
{
	hal_putc(character);
}

/* The kernel's interrupt switch, counted with the hardware layer's. */
bool
kern_irq_disable(void)
{
	irq_disables++;
	return true;
}

void
kern_irq_enable(void)
{
	irq_enables++;
}

/* The test runs on one processor. */
unsigned
kern_cpu_current(void)
{
	return 0U;
}

/* The test's framebuffer is plain memory: nothing to write back or order. */
void
kern_dcache_clean_range(
	const void *address,
	size_t size)
{
	(void)address;
	(void)size;
}

void
kern_io_barrier(void)
{
}

/* The VisionFive 2's other drivers: nothing of the SoC comes up. */

void
disk_registry_reset(void)
{
}

const struct partition_scheme drv_partition_scheme_pcat_auto;

void
partition_set_scheme(
	const struct partition_scheme *scheme)
{
	(void)scheme;
}

int
partition_scan(
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	(void)disk;
	(void)entries;
	(void)capacity;
	return ENODEV;
}

int
drv_ramdisk_create(
	const char *name,
	void *base,
	uint64_t size,
	struct disk **disk_out)
{
	(void)name;
	(void)base;
	(void)size;
	(void)disk_out;
	return ENODEV;
}

int
drv_vf2_syscon_init(
	const struct drv_fdt *fdt)
{
	(void)fdt;
	return ENODEV;
}

int
drv_vf2_crg_init(
	const struct drv_fdt *fdt)
{
	(void)fdt;
	return ENODEV;
}

int
drv_vf2_pinctrl_init(
	const struct drv_fdt *fdt)
{
	(void)fdt;
	return ENODEV;
}

int
drv_vf2_mmc_attach(
	const struct drv_fdt *fdt,
	struct disk **disk)
{
	(void)fdt;
	(void)disk;
	return ENODEV;
}

int
drv_vf2_ethernet_attach(
	const struct drv_fdt *fdt,
	int dma_default_coherent)
{
	(void)fdt;
	(void)dma_default_coherent;
	return ENODEV;
}

int
drv_vf2_pcie_init(
	const struct drv_fdt *fdt,
	int dma_default_coherent)
{
	(void)fdt;
	(void)dma_default_coherent;
	return ENODEV;
}

void
drv_vf2_pcie_input_ready(void)
{
}

void
drv_vf2_pcie_refresh(void)
{
}

int
drv_vf2_pwmdac_attach(
	const struct drv_fdt *fdt,
	int board_default_coherent)
{
	(void)fdt;
	(void)board_default_coherent;
	return ENODEV;
}

int
drv_vf2_pll_get_rate(
	unsigned pll,
	uint64_t *rate)
{
	(void)pll;
	(void)rate;
	return ENODEV;
}

int
drv_vf2_clock_enable(
	const struct drv_vf2_clock *clock)
{
	(void)clock;
	return ENODEV;
}

int
drv_vf2_clock_get_rate(
	const struct drv_vf2_clock *clock,
	uint64_t *rate)
{
	(void)clock;
	(void)rate;
	return ENODEV;
}

int
drv_vf2_clock_running(
	const struct drv_vf2_clock *clock,
	int *running)
{
	(void)clock;
	(void)running;
	return ENODEV;
}

int
drv_vf2_clock_set_input_parent(
	const struct drv_vf2_clock *clock,
	const char *input)
{
	(void)clock;
	(void)input;
	return ENODEV;
}

int
drv_vf2_clock_set_parent(
	const struct drv_vf2_clock *clock,
	const struct drv_vf2_clock *parent)
{
	(void)clock;
	(void)parent;
	return ENODEV;
}

int
drv_vf2_reset_deassert(
	const struct drv_vf2_reset *reset)
{
	(void)reset;
	return ENODEV;
}

int
drv_vf2_reset_released(
	const struct drv_vf2_reset *reset,
	int *released)
{
	(void)reset;
	(void)released;
	return ENODEV;
}

int
drv_vf2_gpio_lookup(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	struct drv_vf2_gpio *gpio)
{
	(void)fdt;
	(void)node;
	(void)name;
	(void)gpio;
	return ENODEV;
}

int
drv_vf2_gpio_configure_input(
	const struct drv_vf2_gpio *gpio)
{
	(void)gpio;
	return ENODEV;
}

int
drv_vf2_gpio_read(
	const struct drv_vf2_gpio *gpio,
	int *active)
{
	(void)gpio;
	(void)active;
	return ENODEV;
}

#if CONFIG_DRIVER_POWERVR
int
drv_vf2_gpu_attach(
	const struct drv_fdt *fdt,
	const struct drv_vf2_gpu_place *builtin)
{
	(void)fdt;
	(void)builtin;
	return ENODEV;
}

int
drv_vf2_pmu_powered(
	unsigned domain,
	int *on)
{
	(void)domain;
	(void)on;
	return ENODEV;
}

int
drv_vf2_pmu_set_power(
	unsigned domain,
	int on)
{
	(void)domain;
	(void)on;
	return ENODEV;
}

int
drv_powervr_dc_probe(
	const struct drv_powervr_dc_place *place)
{
	(void)place;
	return ENODEV;
}

int
drv_powervr_dc_own_mode(
	const struct drv_powervr_dc_own_place *place)
{
	(void)place;
	return ENODEV;
}

int
drv_powervr_dc_own_wanted(void)
{
	return 0;
}

void
drv_powervr_dc_restore_fatal(void)
{
}
#endif

/* Finds the mapping the stub handed out at pointer. */
static struct stub_mapping *
mapping_find(
	void *pointer)
{
	unsigned index;

	/* Each live mapping. */
	for (index = 0U; index < mapping_count; index++) {
		if (mappings[index].pointer == pointer && pointer != NULL)
			return &mappings[index];
	}
	return NULL;
}

/* Enters the native display's graphics mode. */
static int
pc98_enter(
	void *context,
	struct pc98_display_info *info)
{
	(void)context;
	pc98_enters++;
	info->width = 640U;
	info->height = 400U;
	info->bits_per_pixel = 4U;
	info->stride = 80U;
	return 1;
}

/* Leaves the native display's graphics mode. */
static void
pc98_leave(
	void *context)
{
	(void)context;
	pc98_leaves++;
}

/* Fills a rectangle of the native display. */
static int
pc98_fill(
	void *context,
	const struct pc98_display_rect *rect,
	uint32_t color)
{
	(void)context;
	(void)rect;
	(void)color;
	pc98_fills++;
	return 1;
}

/* Reads one UART register: the line status answers as the scene set. */
static uint32_t
uart_read(
	const volatile void *address)
{
	uintptr_t at;
	unsigned number;

	/* The register's number, by the file it lies in. */
	at = (uintptr_t)address;
	number = 0U;
	if (at >= (uintptr_t)uart_console_registers && at < (uintptr_t)(uart_console_registers + 64)) {
		number = (unsigned)((at - (uintptr_t)uart_console_registers) / 4U);
	} else if (at >= (uintptr_t)uart_other_registers && at < (uintptr_t)(uart_other_registers + 64)) {
		number = (unsigned)((at - (uintptr_t)uart_other_registers) / 4U);
	} else if (at >= (uintptr_t)uart_byte_registers && at < (uintptr_t)(uart_byte_registers + 16)) {
		number = (unsigned)(at - (uintptr_t)uart_byte_registers);
	} else {
		printf("FAIL a register read outside the UARTs\n");
		abort();
	}

	/* Only the line status matters. */
	if (number != 5U)
		return 0U;
	uart_status_reads++;
	if (uart_never_ready)
		return 0U;
	if (uart_busy_reads != 0U) {
		uart_busy_reads--;
		return 0U;
	}
	return 0x20U;
}

/* Writes one UART register: the transmitter's bytes are kept. */
static void
uart_write(
	volatile void *address,
	uint32_t value)
{
	uintptr_t at;
	unsigned number;

	/* The register's number, by the file it lies in. */
	at = (uintptr_t)address;
	number = 1U;
	if (at >= (uintptr_t)uart_console_registers && at < (uintptr_t)(uart_console_registers + 64)) {
		number = (unsigned)((at - (uintptr_t)uart_console_registers) / 4U);
	} else if (at >= (uintptr_t)uart_other_registers && at < (uintptr_t)(uart_other_registers + 64)) {
		number = (unsigned)((at - (uintptr_t)uart_other_registers) / 4U);
		uart_other_writes++;
	} else if (at >= (uintptr_t)uart_byte_registers && at < (uintptr_t)(uart_byte_registers + 16)) {
		number = (unsigned)(at - (uintptr_t)uart_byte_registers);
	}

	/* The transmitter holding register. */
	if (number == 0U && uart_sent_count < sizeof(uart_sent))
		uart_sent[uart_sent_count++] = (uint8_t)value;
}
