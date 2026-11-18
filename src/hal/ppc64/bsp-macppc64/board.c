/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC Mac's start: the firmware's answers through the
 * PowerPC Mac's shared support, the switch to the kernel's own hashed
 * table, translation and vectors, the interrupt controller, the display,
 * the power manager, the tick and the kernel; and the processor's
 * start-up of the others, which with one processor running only sums up
 * the bring-up.
 *
 * The firmware enters the image with its own translation on, mapping the
 * image at its own address.  Once the firmware has answered, the start
 * turns translation off and runs at the image's physical address until
 * the HAL's table is in place.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../board.h"
#include "../int.h"
#include "../page.h"
#include "../space.h"
#include "../task.h"
#include "../../powermac/cons.h"
#include "../../powermac/framebuffer.h"
#include "../../powermac/mac99.h"

/*
 * The hashed table's size as a power of two: 1 MiB, 8192 groups, room
 * for the kernel's windows and a cache of user pages well beyond what
 * the RAM the HAL maps can hold.
 */
#define BOARD_TABLE_SHIFT	20U

/*
 * The 970's large page, which the kernel's windows use: 16 MiB.
 *
 * Value referenced: IBM PowerPC 970FX RISC Microprocessor User's Manual
 * Version 1.7 (the large page size).
 */
#define BOARD_LARGE_PAGE_SHIFT	24U

/*
 * The 970's segment lookaside buffer: 64 entries.
 *
 * Value referenced: IBM PowerPC 970FX RISC Microprocessor User's Manual
 * Version 1.7 (the SLB's size).
 */
#define BOARD_SLB_ENTRIES	64U

/*
 * The 970's hardware implementation register 0 and its bit that lets the
 * hypervisor decrementer interrupt.
 *
 * Values referenced: IBM PowerPC 970FX RISC Microprocessor User's Manual
 * Version 1.7 (HID0, SPR 1008, and its hdice bit, bit 23).
 */
#define BOARD_SPR_HID0		1008
#define BOARD_HID0_HDICE	(1UL << (63 - 23))

/* The end of the image, from the linker script. */
extern char __kernel_end[];

/*
 * The hashed table the processor walks, aligned to its size as the
 * table register requires.  It is part of the image's bss, which the
 * page allocator reserves with the image, so no page of it is handed out;
 * the HAL fills it with interrupts off from boot on.
 */
static uint64_t board_table[(1UL << BOARD_TABLE_SHIFT) / sizeof(uint64_t)]
	__attribute__((aligned(1UL << BOARD_TABLE_SHIFT)));

/*
 * What the 64-bit HAL reads about the board, made once at boot from the
 * firmware's answers and read-only afterwards.
 */
static struct ppc64_board_info board_info;

/* Why the display could not be shown, or HAL_OK when it is; set once at boot. */
static int board_display_error = HAL_ERR_UNSUPPORTED;

static void fill_board_info(const struct mac99_board_info *board);
static void setup_970(void);
static void describe_bringup(void);

/*
 * The C entry, from entry.S on the firmware's translation and stack, with
 * the initial ramdisk's address and size the loader passed in r3 and r4
 * (in 32-bit mode: their low words).
 */
void
ppc64_cmain(
	uint64_t initrd_address,
	uint64_t initrd_size)
{
	const struct mac99_board_info *board;
	const struct mac99_interrupt_route *routes;
	unsigned route_count;

	/* The firmware's answers, while it can still give them. */
	mac99_firmware_read((uint32_t)initrd_address, (uint32_t)initrd_size);
	board = mac99_board();
	fill_board_info(board);
	hal_puts("PPC64 ENTRY\n");
	hal_printf("PPC64 RAM %llu MiB IN BANKS, %u MiB FROM %08x\n",
		   board->memory_total / (1024U * 1024U), board->memory_size / (1024U * 1024U), board->memory_base);

	/* The firmware is done: the image runs at its physical address from here. */
	ppc64_write_msr(ppc64_read_msr() & ~(uint64_t)(PPC64_MSR_IR | PPC64_MSR_DR));

	/* The processor's own settings. */
	setup_970();

	/* The kernel's vectors, its own table and translation. */
	ppc64_int_init();
	ppc64_space_init(&board_info);

	/* The interrupt controller, every source masked until a driver asks for it. */
	routes = mac99_interrupt_routes(&route_count);
	mac99_openpic_init(board->openpic_base, routes, route_count);
	if (board->pmu_gpio_interrupt != 0xffffffffU)
		mac99_openpic_sense(board->pmu_gpio_interrupt, board->pmu_gpio_sense);

	/* The pages, below the image's end. */
	ppc64_page_init((uint32_t)(uintptr_t)__kernel_end);

	/* The display, which draws what the console printed before it. */
	board_display_error = virt_framebuffer_init(board->framebuffer_address, board->framebuffer_width,
						    board->framebuffer_height, board->framebuffer_linebytes,
						    board->framebuffer_depth);
	if (board_display_error == HAL_OK) {
		virt_cons_attach();
		virt_framebuffer_describe();
	}

	/* The power manager and its clock, the tick, then the switch's self-test. */
	mac99_pmu_init();
	mac99_clock_init();
	ppc64_context_selftest();

	/* The kernel; it does not come back. */
	kernel_entry(mac99_kernel_handoff());
	HAL_FATAL("ppc64 kernel_entry returned");
}

/*
 * Reports what the 64-bit HAL reads about the board.
 */
const struct ppc64_board_info *
ppc64_board(
	void)
{
	/* Succeeded: the record made at boot. */
	return &board_info;
}

/*
 * Starts the tick.
 */
void
ppc64_clock_init(
	void)
{
	/* The PowerPC Mac's decrementer tick. */
	mac99_clock_init();
}

/*
 * Serves the decrementer's interrupt.
 */
void
ppc64_clock_interrupt(
	void)
{
	/* The PowerPC Mac's tick. */
	mac99_clock_interrupt();
}

/*
 * Reports whether the first tick has come.
 */
bool
ppc64_clock_ticked(
	void)
{
	bool ticked;

	/* Waits a short while for it. */
	ticked = mac99_clock_ticked();

	/* Succeeded: whether it came. */
	return ticked;
}

/*
 * Serves an external interrupt.
 */
void
ppc64_take_external(
	void)
{
	/* The interrupt controller names the source and its handler ends it. */
	mac99_take_external();
}

/*
 * Starts the other processors: only the first runs yet, so it sums up
 * the bring-up, which is the HAL's last step of the boot.
 */
int
hal_cpu_start_others(
	void)
{
	/* The line stays in view after the kernel's own lines. */
	describe_bringup();
	hal_puts("PPC64 CPU 1 ONLINE\n");
	return HAL_OK;
}

/*
 * Restarts the machine through the power manager; without one, or if it
 * does not answer, it stops.
 */
void
hal_reset(
	void)
{
	/* Asks the power manager, then stops with interrupts off. */
	mac99_pmu_restart();
	ppc64_halt_forever();
}

/*
 * Turns the machine off through the power manager; without one, or if it
 * does not answer, it stops.
 */
void
hal_poweroff(
	void)
{
	/* Asks the power manager, then stops with interrupts off. */
	mac99_pmu_poweroff();
	ppc64_halt_forever();
}

/*
 * Makes the 64-bit HAL's record of the board from the firmware's
 * answers: the RAM, the time base, the display and the device windows,
 * and the hashed table the processor runs directly with its 16 MiB page.
 */
static void
fill_board_info(
	const struct mac99_board_info *board)
{
	unsigned index;

	/* The RAM and the time base. */
	hal_memset(&board_info, 0, sizeof(board_info));
	board_info.memory_base = board->memory_base;
	board_info.memory_size = board->memory_size;
	board_info.ramdisk_base = board->ramdisk_base;
	board_info.ramdisk_size = board->ramdisk_size;
	board_info.timebase_frequency = board->timebase_frequency;

	/* The table the HAL keeps itself and the kernel's large page. */
	board_info.under_hypervisor = 0;
	board_info.htab_base = (uint64_t)(uintptr_t)board_table;
	board_info.htab_shift = BOARD_TABLE_SHIFT;
	board_info.large_page_shift = BOARD_LARGE_PAGE_SHIFT;
	board_info.slb_entries = BOARD_SLB_ENTRIES;

	/* The display. */
	board_info.framebuffer_address = board->framebuffer_address;
	board_info.framebuffer_width = board->framebuffer_width;
	board_info.framebuffer_height = board->framebuffer_height;
	board_info.framebuffer_linebytes = board->framebuffer_linebytes;
	board_info.framebuffer_depth = board->framebuffer_depth;

	/* The device windows, in the firmware's order. */
	for (index = 0; index < board->window_count && index < PPC64_BOARD_WINDOWS_MAX; index++)
		board_info.windows[index] = board->windows[index];
	board_info.window_count = index;
}

/*
 * Sets the 970 up the way the HAL runs it: the hypervisor decrementer,
 * which the HAL does not use, may not interrupt.  The register is written
 * with the sequence the processor asks for: a sync before, and six reads
 * after.
 *
 * Procedure referenced: IBM PowerPC 970FX RISC Microprocessor User's
 * Manual Version 1.7 (the HID0 update sequence).
 */
static void
setup_970(
	void)
{
	uint64_t hid0;

	/* The register as the firmware left it, without the decrementer's enable. */
	__asm__ volatile("mfspr %0,%1" : "=r"(hid0) : "i"(BOARD_SPR_HID0));
	hid0 &= ~BOARD_HID0_HDICE;
	__asm__ volatile("sync\n\t"
			 "mtspr %1,%0\n\t"
			 "mfspr %0,%1\n\t"
			 "mfspr %0,%1\n\t"
			 "mfspr %0,%1\n\t"
			 "mfspr %0,%1\n\t"
			 "mfspr %0,%1\n\t"
			 "mfspr %0,%1\n\t"
			 "isync"
			 : "+r"(hid0) : "i"(BOARD_SPR_HID0) : "memory");
}

/*
 * Writes one line that sums up the bring-up: the display, translation and
 * the tick.
 */
static void
describe_bringup(
	void)
{
	const char *display;
	const char *timer;
	bool ticked;

	/* Words the display's state and whether the tick has arrived. */
	display = "FIRMWARE";
	if (board_display_error != HAL_OK)
		display = "NONE";
	timer = "NO TICK";
	ticked = mac99_clock_ticked();
	if (ticked)
		timer = "TICK";

	/* Writes the line. */
	hal_printf("PPC64 HAL READY DISPLAY %s PAGING PASS TIMER %s\n", display, timer);
}
