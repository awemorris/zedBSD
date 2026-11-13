/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 boot processor's first C code: from locore.S to the kernel.
 *
 * The processor's record comes first and then the board's console, so
 * that everything after can report.  The board names the device tree (the
 * loader's r2, or where the board's loader puts it), which is read through
 * the boot map's direct map; RAM the board keeps out (ARMV7_LOW_RAM_SKIP)
 * is taken out of what it describes.  The board's display is set up, the
 * run-time ABI and the exclusives are checked, the floating-point unit is
 * given to the modes and turned off, then physical memory, the kernel's
 * table, the interrupt controller and the tick, which is seen running
 * before the kernel starts.  The kernel calls back into
 * hal_cpu_start_others() once its heap exists, where the address spaces
 * and the floating-point switch are checked and the bring-up is summed up.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "bsp.h"
#include "clock.h"
#include "defs.h"
#include "fdt.h"
#include "fpu.h"
#include "int.h"
#include "irq.h"
#include "page.h"
#include "percpu.h"
#include "selftest.h"
#include "space.h"
#include "task.h"

/* The most bytes of the loader's device tree the parser may read. */
#define CMAIN_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/*
 * The first word of a flattened device tree as it lies in memory, read as
 * a little-endian word: the big-endian magic 0xd00dfeed.
 *
 * Values referenced: Devicetree Specification (devicetree.org), the
 * flattened format's magic.
 */
#define CMAIN_FDT_MAGIC_LE	0xedfe0dd0U

/* The ticks the boot waits to see before it reports the tick as running. */
#define CMAIN_TICKS_WANTED	10U

/* How many tick intervals the boot allows for each tick it waits for. */
#define CMAIN_TICK_WAIT_FACTOR	10U

/*
 * The boot parameter that asks the boot to time ten seconds of ticks, so
 * the tick's rate can be checked against a clock outside the machine, and
 * how long that takes.
 */
#define CMAIN_TIMER_CHECK_PARAMETER	"armv7.timer-check"
#define CMAIN_TIMER_CHECK_SECONDS	10U

/* The bounds of the kernel image, from the linker script. */
extern char __kernel_vma_start[];
extern char __kernel_vma_end[];

/*
 * The device tree's record while it is parsed, before the boot record
 * keeps a copy.  It is too large for the boot stack.
 */
static struct armv7_fdt_info cmain_fdt;

static void check_boot_argument(uintptr_t fdt_phys);
static void check_overlap(uint64_t base, uint64_t size, const char *what);
static void keep_out_low_ram(struct armv7_fdt_info *info);
static void report_pending_abort(void);
static void wait_for_ticks(void);
static void time_ten_seconds(void);

/*
 * Brings the boot processor from locore.S to kernel_entry().
 *
 * The loader passes the device tree's physical address in r2 (which the
 * board may replace with the address its loader uses), and locore.S where
 * the image was loaded.
 */
void
armv7_cmain(
	uintptr_t fdt_phys,
	uintptr_t kernel_phys_start)
{
	int error;

	/* The image's place and the boot processor's record come first: everything after may take a lock. */
	armv7_kernel_image_init(kernel_phys_start);
	armv7_smp_bootstrap();

	/* The board's console, at the address the board knows, before anything reports. */
	bsp_early_init();

	/* The board names the device tree, which must lie inside the boot map. */
	fdt_phys = bsp_find_fdt(fdt_phys);
	check_boot_argument(fdt_phys);

	/* Reads the tree through the boot map's direct map. */
	error = armv7_fdt_parse(armv7_phys_to_direct((uint64_t)fdt_phys), CMAIN_FDT_READ_LIMIT, &cmain_fdt);
	if (error != ARMV7_FDT_OK) {
		hal_printf("armv7 device tree: %s\n", armv7_fdt_error(error));
		HAL_FATAL("invalid armv7 device tree");
	}

	/* The RAM the board keeps out is no RAM to the rest of the HAL. */
	keep_out_low_ram(&cmain_fdt);

	/* The tree and the ramdisk must lie outside the image, which overwrote them otherwise. */
	check_overlap((uint64_t)fdt_phys, cmain_fdt.totalsize, "device tree");
	if (cmain_fdt.initrd_end > cmain_fdt.initrd_start)
		check_overlap(cmain_fdt.initrd_start, cmain_fdt.initrd_end - cmain_fdt.initrd_start, "initial ramdisk");

	/* Keeps the boot record and prepares the kernel's handoff. */
	armv7_boot_record(fdt_phys, &cmain_fdt);

	/* Shows the display, which then holds what the console printed. */
	bsp_init(armv7_boot_fdt());

	/* Describes what was found. */
	hal_printf("ARMV7 ENTRY midr=%08x mpidr=%08x sctlr=%08x image=%08x\n",
		   armv7_read_midr(),
		   armv7_read_mpidr(),
		   armv7_read_sctlr(),
		   (uint32_t)kernel_phys_start);
	hal_printf("ARMV7 FDT PASS phys=%08x size=%u cpus=%u initrd=%llx-%llx\n",
		   (uint32_t)fdt_phys,
		   cmain_fdt.totalsize,
		   cmain_fdt.cpu_count,
		   cmain_fdt.initrd_start,
		   cmain_fdt.initrd_end);
	bsp_describe();
	report_pending_abort();

	/* The run-time ABI's helpers and the exclusives, which the kernel's code relies on. */
	armv7_aeabi_selftest();
	armv7_atomic_selftest();

	/* The floating-point unit, given to user contexts lazily and off until one uses it. */
	armv7_fpu_init();

	/* Physical memory, then the kernel's table, then the task switch. */
	armv7_page_init();
	armv7_space_init();
	armv7_context_selftest();

	/* The interrupt controller with every line masked, then the tick, seen running. */
	armv7_irq_init();
	armv7_clock_init();
	wait_for_ticks();
	time_ten_seconds();

	/*
	 * Hands the ticks to the kernel and enters it with interrupts masked:
	 * the kernel unmasks them once what a tick reaches is set up.  It
	 * never returns.
	 */
	(void)armv7_irq_save();
	armv7_clock_start_kernel();
	kernel_entry(armv7_kernel_handoff());
	HAL_FATAL("kernel_entry returned");
}

/*
 * Stops at a boot argument that is not a device tree inside the boot
 * map: the loader is not one this entry is for.
 */
static void
check_boot_argument(
	uintptr_t fdt_phys)
{
	const volatile uint32_t *first_word;
	uint32_t magic;

	/* The tree must be inside the RAM the boot map reaches. */
	if (fdt_phys == 0)
		HAL_FATAL("armv7 loader passed no device tree");
	if ((uint64_t)fdt_phys < (uint64_t)ARMV7_PHYS_BASE + ARMV7_LOW_RAM_SKIP)
		HAL_FATAL("armv7 device tree below the direct map");
	if ((uint64_t)fdt_phys - ARMV7_PHYS_BASE >= (uint64_t)ARMV7_BOOT_MAP_BYTES - CMAIN_FDT_READ_LIMIT)
		HAL_FATAL("armv7 device tree beyond the boot map");

	/* Its first word must be the tree's magic. */
	first_word = (const volatile uint32_t *)armv7_phys_to_direct((uint64_t)fdt_phys);
	magic = *first_word;
	if (magic != CMAIN_FDT_MAGIC_LE) {
		hal_printf("armv7 boot argument %08x holds %08x\n", (uint32_t)fdt_phys, magic);
		HAL_FATAL("armv7 boot argument is not a device tree");
	}
}

/*
 * Stops at a range the loader placed that overlaps the kernel image: the
 * image's zeroed data has already overwritten it.
 */
static void
check_overlap(
	uint64_t base,
	uint64_t size,
	const char *what)
{
	uint64_t image_start;
	uint64_t image_end;

	/* The image's physical range, its zeroed data included. */
	image_start = armv7_kernel_phys_start();
	image_end = image_start + (uint64_t)(uintptr_t)(__kernel_vma_end - __kernel_vma_start);

	/* A range wholly below or wholly above the image is clear of it. */
	if (base + size <= image_start)
		return;
	if (base >= image_end)
		return;

	/* Otherwise the range was lost. */
	hal_printf("armv7 %s %llx+%llx overlaps the image %llx-%llx\n", what, base, size, image_start, image_end);
	HAL_FATAL("armv7 loader placed data inside the kernel image");
}

/*
 * Takes the RAM the board keeps out (the first ARMV7_LOW_RAM_SKIP bytes
 * of RAM) off the device tree's banks: a bank wholly inside it is dropped,
 * and one that starts inside it starts past it.
 */
static void
keep_out_low_ram(
	struct armv7_fdt_info *info)
{
	uint64_t floor;
	uint64_t end;
	unsigned index;
	unsigned kept;

	/* RAM below the floor is the board's own. */
	floor = (uint64_t)ARMV7_PHYS_BASE + ARMV7_LOW_RAM_SKIP;

	/* Keeps each bank's part above the floor, in the banks' order. */
	kept = 0;
	for (index = 0; index < info->memory_count; index++) {
		/* A bank that ends at or below the floor is dropped. */
		end = info->memory[index].base + info->memory[index].size;
		if (end <= floor)
			continue;

		/* A bank that starts below the floor starts at it. */
		info->memory[kept].base = info->memory[index].base;
		info->memory[kept].size = info->memory[index].size;
		if (info->memory[kept].base < floor) {
			info->memory[kept].base = floor;
			info->memory[kept].size = end - floor;
		}

		/* The bank is kept. */
		kept++;
	}

	/* Succeeded: only the banks above the floor are left. */
	info->memory_count = kept;
}

/*
 * Writes the asynchronous abort the loader left pending to the boot log,
 * when the boot took one as it unmasked them.
 */
static void
report_pending_abort(
	void)
{
	uint32_t status;
	uint32_t address;
	int seen;

	/* Asks the trap code whether one was taken. */
	seen = armv7_trap_pending_abort(&status, &address);
	if (!seen)
		return;

	/* Succeeded: the abort is on record and the boot went past it. */
	hal_printf("ARMV7 PENDING ASYNC ABORT DFSR=%08x DFAR=%08x\n", status, address);
}

/*
 * Lets the tick in and waits a bounded time, by the counter, for the
 * first ticks.
 */
static void
wait_for_ticks(
	void)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t limit;
	unsigned long ticks;
	bool read;

	/* Reads the counter the wait is measured by. */
	read = armv7_clock_read(&start, &frequency);
	if (!read || frequency == 0)
		HAL_FATAL("armv7 board counter unreadable");
	limit = frequency / ARMV7_TICK_HZ * CMAIN_TICK_WAIT_FACTOR * CMAIN_TICKS_WANTED;

	/* Waits with interrupts unmasked until enough ticks came, or the time is up. */
	__asm__ volatile("cpsie i" : : : "memory");
	for (;;) {
		ticks = armv7_clock_ticks();
		if (ticks >= CMAIN_TICKS_WANTED)
			break;
		(void)armv7_clock_read(&now, &frequency);
		if (now - start > limit) {
			hal_printf("ARMV7 TIMER FAIL ticks=%u\n", (uint32_t)ticks);
			armv7_halt_forever();
		}

		/* Spins politely until the next look. */
		hal_atomic_relax();
	}

	/* Masks interrupts again, as the boot runs with them masked. */
	__asm__ volatile("cpsid i" : : : "memory");

	/* Reports the tick with the rate its counter runs at. */
	hal_printf("ARMV7 TIMER TICK ticks=%u counter=%llu Hz irq=%u\n",
		   (uint32_t)ticks,
		   frequency,
		   armv7_clock_tick_irq());
}

/*
 * Times ten seconds of ticks when the command line asks for it: one line
 * as the count starts and one when the ticks of ten seconds have come,
 * with how far the counter moved meanwhile.  The two lines' arrival on
 * the serial line, measured by the host's clock, checks the tick's rate.
 */
static void
time_ten_seconds(
	void)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t limit;
	unsigned long first_tick;
	unsigned long ticks;
	unsigned long wanted;
	int requested;

	/* Only a boot that asks waits the ten seconds. */
	requested = armv7_boot_has_parameter(CMAIN_TIMER_CHECK_PARAMETER);
	if (!requested)
		return;

	/* The ticks of ten seconds, waited for at most twice as long by the counter. */
	wanted = (unsigned long)CMAIN_TIMER_CHECK_SECONDS * ARMV7_TICK_HZ;
	(void)armv7_clock_read(&start, &frequency);
	limit = frequency * CMAIN_TIMER_CHECK_SECONDS * 2U;
	first_tick = armv7_clock_ticks();
	hal_puts("ARMV7 TIMER 0S\n");

	/* Counts the ticks with interrupts unmasked. */
	__asm__ volatile("cpsie i" : : : "memory");
	for (;;) {
		ticks = armv7_clock_ticks() - first_tick;
		if (ticks >= wanted)
			break;
		(void)armv7_clock_read(&now, &frequency);
		if (now - start > limit)
			break;
		hal_atomic_relax();
	}

	/* Masks interrupts again, as the boot runs with them masked. */
	__asm__ volatile("cpsid i" : : : "memory");

	/* Reports the ticks counted and the counter's distance, in its own units. */
	(void)armv7_clock_read(&now, &frequency);
	hal_printf("ARMV7 TIMER %uS ticks=%u counter=%llu Hz=%llu\n",
		   (uint32_t)CMAIN_TIMER_CHECK_SECONDS,
		   (uint32_t)ticks,
		   now - start,
		   frequency);
}
