/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's start: the firmware's answers, the processor's family, the
 * switch to the kernel's translation and vectors, the idle wait's power
 * saving, the display, the power manager, the tick and the kernel; and
 * the processor's start-up of the others, which on this uniprocessor
 * board only sums up the bring-up.  Restarting and turning off go
 * through the power manager.
 *
 * The idle wait saves power (doze, or nap on the 7450 family) unless the
 * firmware's command line has the word ppc32.powersave=off.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../int.h"
#include "../page.h"
#include "../space.h"
#include "../task.h"
#include "../../powermac/mac99.h"
#include "../../powermac/cons.h"
#include "../../powermac/framebuffer.h"

/* The end of the image, from the linker script. */
extern char __kernel_end[];

/* Why the display could not be shown, or HAL_OK when it is. */
static int board_display_error = HAL_ERR_UNSUPPORTED;

static void describe_bringup(void);

/*
 * The C entry, from entry.S on the firmware's translation and stack, with
 * the initial ramdisk's address and size the loader passed in r3 and r4.
 */
void
ppc32_cmain(
	uint32_t initrd_address,
	uint32_t initrd_size)
{
	const struct mac99_board_info *board;
	const struct mac99_interrupt_route *routes;
	unsigned route_count;
	bool powersave_off;

	/* The firmware's answers, while it can still give them. */
	mac99_firmware_read(initrd_address, initrd_size);
	board = mac99_board();
	hal_puts("PPC32 ENTRY\n");
	hal_printf("PPC32 RAM %llu MiB IN BANKS, %u MiB FROM %08x\n",
		   board->memory_total / (1024U * 1024U), board->memory_size / (1024U * 1024U), board->memory_base);

	/* The processor, then the kernel's translation and vectors; the firmware is gone from here on. */
	ppc32_cpu_identify();
	ppc32_space_init((uint64_t)board->memory_base + board->memory_size, board->windows, board->window_count);
	ppc32_int_init();

	/* The interrupt controller, every source masked until a driver asks for it. */
	routes = mac99_interrupt_routes(&route_count);
	mac99_openpic_init(board->openpic_base, routes, route_count);
	if (board->pmu_gpio_interrupt != 0xffffffffU)
		mac99_openpic_sense(board->pmu_gpio_interrupt, board->pmu_gpio_sense);

	/* The idle wait's power saving, unless the command line turns it off. */
	powersave_off = mac99_boot_option("ppc32.powersave=off");
	ppc32_cpu_power_init(!powersave_off);

	/* The pages, below the image's end. */
	ppc32_page_init((uint32_t)(uintptr_t)__kernel_end);

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
	ppc32_context_selftest();

	/* The kernel; it does not come back. */
	kernel_entry(mac99_kernel_handoff());
	HAL_FATAL("ppc32 kernel_entry returned");
}

/*
 * Starts the other processors: there are none, so it sums up the
 * bring-up, which is the HAL's last step of the boot.
 */
int
hal_cpu_start_others(
	void)
{
	/* The line stays in view after the kernel's own lines. */
	describe_bringup();
	hal_puts("PPC32 CPU 1 ONLINE\n");
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
	ppc32_halt_forever();
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
	ppc32_halt_forever();
}

/*
 * Writes cached lines of a range back to memory.
 */
void
hal_dcache_clean_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;

	/* Each line of the range, then the ordering. */
	for (line = address & ~(uintptr_t)31U; line < address + size; line += 32U)
		__asm__ volatile("dcbst 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Discards cached lines of a range; they are written back first, which
 * keeps the data of lines the range shares.
 */
void
hal_dcache_invalidate_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;

	/* Each line of the range, then the ordering. */
	for (line = address & ~(uintptr_t)31U; line < address + size; line += 32U)
		__asm__ volatile("dcbf 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Writes back and discards cached lines of a range.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t address,
	size_t size)
{
	/* The same as the discard. */
	hal_dcache_invalidate_range(address, size);
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
	hal_printf("PPC32 HAL READY DISPLAY %s PAGING PASS TIMER %s\n", display, timer);
}
