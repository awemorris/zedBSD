/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's terminal line, its control device (reset and power),
 * the boot-time start of the HAL, and the processor's start-up of the
 * others, which on this uniprocessor board only sums up the bring-up.
 *
 * Values referenced: QEMU v10.0.0 hw/char/goldfish_tty.c (the put-char
 * register) and hw/misc/virt_ctrl.c (the command register and its reset
 * and halt commands).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "../m68040/int.h"
#include "../m68040/page.h"
#include "../m68040/space.h"
#include "../m68040/task.h"
#include "bsp.h"
#include "cons.h"
#include "framebuffer.h"

/* The terminal's put-char register and the control device's command register. */
#define TTY_PUT_CHAR		0x00U
#define CTRL_CMD		0x04U
#define CTRL_CMD_RESET		1U
#define CTRL_CMD_HALT		2U

/* Why the display could not be shown, or HAL_OK when it is. */
static int board_display_error = HAL_ERR_UNSUPPORTED;

static void describe_bringup(void);

/*
 * The C entry, from start.S with the boot information's address: reads
 * the board, turns translation and the caches on, shows the display,
 * starts the interrupts and the tick, and enters the kernel.
 */
void
virt_cmain(
	const void *records)
{
	uint32_t records_end;

	/* The board first: everything else uses its addresses. */
	records_end = virt_bootinfo_parse(records);
	hal_puts("M68040 ENTRY\n");

	/* The exceptions, the pages, translation and the caches. */
	m68040_int_init();
	m68040_page_init(records_end);
	m68040_space_init();
	m68040_cache_enable();

	/* The display, which draws what the console printed before it. */
	board_display_error = virt_framebuffer_init();
	if (board_display_error == HAL_OK) {
		virt_cons_attach();
		virt_framebuffer_describe();
	} else {
		hal_printf("M68K VIRT FRAMEBUFFER unavailable (%u)\n", (uint32_t)board_display_error);
	}

	/* The interrupt controllers and the tick, then the switch's self-test. */
	virt_pic_init();
	virt_clock_init();
	m68040_context_selftest();

	/* The kernel; it does not come back. */
	kernel_entry(virt_kernel_handoff());
	HAL_FATAL("m68k virt kernel_entry returned");
}

/*
 * Writes one character to the board's terminal line, when it has one.
 */
void
virt_tty_putc(
	char character)
{
	uint32_t base;

	/* The line is optional; the screen is the console. */
	base = virt_board()->tty_base;
	if (base == 0)
		return;
	*(volatile uint32_t *)(uintptr_t)(base + TTY_PUT_CHAR) = (uint8_t)character;
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
 * Restarts the machine through the control device.
 */
void
hal_reset(
	void)
{
	uint32_t base;

	/* The command, then a stop in case it did not act. */
	(void)hal_irq_disable();
	base = virt_board()->ctrl_base;
	if (base != 0)
		*(volatile uint32_t *)(uintptr_t)(base + CTRL_CMD) = CTRL_CMD_RESET;
	m68040_halt_forever();
}

/*
 * Turns the machine off through the control device.
 */
void
hal_poweroff(
	void)
{
	uint32_t base;

	/* The command, then a stop in case it did not act. */
	(void)hal_irq_disable();
	base = virt_board()->ctrl_base;
	if (base != 0)
		*(volatile uint32_t *)(uintptr_t)(base + CTRL_CMD) = CTRL_CMD_HALT;
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
	display = "VIRTIO-GPU";
	if (board_display_error != HAL_OK)
		display = "NONE";
	timer = "NO TICK";
	ticked = virt_clock_ticked();
	if (ticked)
		timer = "TICK";

	/* Writes the line. */
	hal_printf("M68040 HAL READY DISPLAY %s PAGING PASS TIMER %s\n", display, timer);
}
