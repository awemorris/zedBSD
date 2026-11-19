/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh's boot-time start of the HAL, its RAM and reserved ranges
 * for the page allocator, the machine's power and reset, the data cache
 * maintenance, and the processor's start-up of the others, which on this
 * uniprocessor machine only sums up the bring-up.
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "../m68040/int.h"
#include "../m68040/page.h"
#include "../m68040/space.h"
#include "../m68040/task.h"
#include "bsp.h"

/* Why the screen could not be drawn on, or HAL_OK when it is. */
static int board_display_error = HAL_ERR_UNSUPPORTED;

static void describe_machine(void);
static void describe_bringup(void);

/*
 * The C entry, from start.S with the boot information's address: reads
 * the machine, shows the screen, quiets the interrupts, turns translation
 * and the caches on, starts the tick, and enters the kernel.
 */
void
mac68k_cmain(
	const void *records)
{
	uint32_t records_end;

	/* The machine first: everything else uses what the loader said. */
	records_end = mac68k_bootinfo_parse(records);

	/* The screen, which then shows what the console keeps. */
	board_display_error = mac68k_framebuffer_init();
	if (board_display_error == HAL_OK)
		mac68k_cons_attach();
	hal_puts("M68040 ENTRY\n");
	describe_machine();
	if (board_display_error == HAL_OK) {
		mac68k_framebuffer_describe();
	} else {
		hal_printf("MAC68K FRAMEBUFFER unavailable (%u)\n", (uint32_t)board_display_error);
	}

	/* The ROM's interrupt sources fall silent before anything can let them in. */
	mac68k_via_init();

	/* The exceptions, the pages, translation and the caches. */
	m68040_int_init();
	m68040_page_init(records_end);
	m68040_space_init();
	m68040_cache_enable();

	/* The tick, then the task switch's self-test. */
	mac68k_clock_init();
	m68040_context_selftest();

	/* The kernel; it does not come back. */
	kernel_entry(mac68k_kernel_handoff());
	HAL_FATAL("mac68k kernel_entry returned");
}

/*
 * Reports the machine's RAM: the banks of the boot information.
 */
unsigned
m68040_board_memory(
	const struct m68040_memory_bank **banks)
{
	const struct mac68k_board_info *board;

	/* The banks as the loader listed them. */
	board = mac68k_board();
	*banks = board->memory_banks;

	/* Succeeded: how many there are. */
	return board->memory_bank_count;
}

/*
 * Reports a range the machine keeps out of the page allocator: the
 * initial ramdisk, the only one.
 */
bool
m68040_board_reserved(
	unsigned index,
	uint32_t *base,
	uint32_t *size)
{
	const struct mac68k_board_info *board;

	/* There is one range at most. */
	if (index != 0)
		return false;

	/* Succeeded: the ramdisk, empty when the loader placed none. */
	board = mac68k_board();
	*base = board->ramdisk_base;
	*size = board->ramdisk_size;
	return true;
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
	hal_puts("M68040 CPU 1 ONLINE\n");
	return HAL_OK;
}

/*
 * Restarts the machine.  The HAL does not drive the machine's reset yet,
 * so it stops.
 */
void
hal_reset(
	void)
{
	/* Stops with interrupts off. */
	(void)hal_irq_disable();
	hal_puts("MAC68K RESET unsupported; halted\n");
	m68040_halt_forever();
}

/*
 * Turns the machine off where the HAL can, and stops otherwise.
 */
void
hal_poweroff(
	void)
{
	/* The power bit, then a stop in case it did not act. */
	(void)hal_irq_disable();
	mac68k_via_power_off();
	m68040_halt_forever();
}

/*
 * Writes cached lines of a range back to memory: the whole data cache is
 * pushed.
 */
void
hal_dcache_clean_range(
	uintptr_t address,
	size_t size)
{
	UNUSED_PARAMETER(address);

	/* An empty range needs nothing. */
	if (size != 0)
		m68040_cache_push_all();
}

/*
 * Discards cached lines of a range: the whole cache is pushed and emptied,
 * which also keeps lines outside the range.
 */
void
hal_dcache_invalidate_range(
	uintptr_t address,
	size_t size)
{
	UNUSED_PARAMETER(address);

	/* An empty range needs nothing. */
	if (size != 0)
		m68040_cache_push_all();
}

/*
 * Writes back and discards cached lines of a range.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t address,
	size_t size)
{
	UNUSED_PARAMETER(address);

	/* An empty range needs nothing. */
	if (size != 0)
		m68040_cache_push_all();
}

/* Writes one line that names the machine and its memory. */
static void
describe_machine(
	void)
{
	const struct mac68k_board_info *board;
	uint32_t total;
	uint32_t index;

	/* Adds up the banks. */
	board = mac68k_board();
	total = 0;
	for (index = 0; index < board->memory_bank_count; index++)
		total += board->memory_banks[index].size;

	/* Writes the line. */
	hal_printf("MAC68K MODEL %u MEMORY %u KiB IN %u BANKS\n",
		   board->model,
		   total / 1024U,
		   board->memory_bank_count);
}

/*
 * Writes one line that sums up the bring-up: the screen, translation and
 * the tick.
 */
static void
describe_bringup(
	void)
{
	const char *display;
	const char *timer;
	bool ticked;

	/* Words the screen's state and whether the tick has arrived. */
	display = "FRAMEBUFFER";
	if (board_display_error != HAL_OK)
		display = "NONE";
	timer = "NO TICK";
	ticked = mac68k_clock_ticked();
	if (ticked)
		timer = "TICK";

	/* Writes the line. */
	hal_printf("M68040 HAL READY DISPLAY %s PAGING PASS TIMER %s\n", display, timer);
}
