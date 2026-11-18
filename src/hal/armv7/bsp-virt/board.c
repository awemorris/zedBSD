/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The emulator's virt board for a 32-bit ARM processor.
 *
 * Everything the kernel needs is in the device tree but the display: the
 * board shows a RAM framebuffer, which the HAL sets up through the
 * firmware configuration interface and draws its early console on, beside
 * the PL011.  The processor enters non-secure, and the GIC has no
 * security extensions.  The loader's own code lies in the first 64 KiB of
 * RAM, below the image.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../defs.h"
#include "cons.h"
#include "framebuffer.h"
#include "fwcfg.h"
#include "virt.h"

/*
 * The RAM the emulator's loader occupies, which it entered the image from.
 *
 * Values referenced: QEMU v10.0.11 hw/arm/boot.c, the bootloader placed
 * at the start of RAM and the kernel at KERNEL_LOAD_ADDR (0x10000) above
 * it.
 */
#define VIRT_LOADER_BYTES	0x10000U

/*
 * Why the display could not be shown, or HAL_OK when it is.  Kept for the
 * boot log; set once on the boot processor.
 */
static int board_display_error = HAL_ERR_UNSUPPORTED;

/*
 * Makes the console reachable before the device tree is read: the PL011
 * at the address the board always has it.
 */
void
bsp_early_init(
	void)
{
	/* A failure leaves the console silent until the screen is shown. */
	(void)virt_uart_init(VIRT_UART_PHYS);
}

/*
 * Names the device tree: the one the emulator's loader passes in r2.
 */
uintptr_t
bsp_find_fdt(
	uintptr_t loader_argument)
{
	/* Succeeded: the loader's argument, which the boot checks. */
	return loader_argument;
}

/*
 * Sets up what the device tree describes: the UART it names, the firmware
 * call conduit, and the display, on which the early console's lines are
 * then drawn.
 */
void
bsp_init(
	const struct armv7_fdt_info *info)
{
	int error;

	/* Moves the console to the UART the tree names, if it is another. */
	if (info->uart.size != 0 && info->uart.base != VIRT_UART_PHYS)
		(void)virt_uart_init(info->uart.base);

	/* The conduit of the reset and the power off. */
	virt_psci_init(info);

	/* Finds the configuration interface the device tree names. */
	error = virt_fwcfg_init(info->fwcfg.base, info->fwcfg.size);
	if (error != HAL_OK) {
		board_display_error = error;
		return;
	}

	/* Points the framebuffer device at the screen's memory. */
	error = virt_framebuffer_init();
	board_display_error = error;
	if (error != HAL_OK)
		return;

	/* Draws what the console printed before the screen was shown. */
	virt_cons_attach();
}

/*
 * Writes the board's display to the boot log.
 */
void
bsp_describe(
	void)
{
	/* Describes the screen, or why there is none. */
	if (board_display_error == HAL_OK) {
		virt_framebuffer_describe();
		return;
	}

	/* Otherwise reports why there is no screen. */
	hal_printf("ARMV7 FRAMEBUFFER unavailable (%u); serial console only\n",
		   (uint32_t)board_display_error);
}

/*
 * Writes one early-console character to the UART and the screen.
 */
void
bsp_cons_putc(
	int character)
{
	/* The UART, then the screen, which keeps the character whether or not it is shown yet. */
	virt_uart_putc(character);
	virt_cons_putc(character);
}

/*
 * Reads one character from the UART, or -1 when none has arrived.
 */
int
bsp_cons_getc(
	void)
{
	int character;

	/* Polls the receive FIFO. */
	character = virt_uart_getc();

	/* Succeeded: the character, or -1. */
	return character;
}

/*
 * Reports the GIC's two blocks from the device tree; the processor is
 * non-secure on this board.
 */
int
bsp_gic_bases(
	const struct armv7_fdt_info *info,
	uint64_t *distributor,
	uint64_t *cpu_interface,
	int *secure)
{
	/* A tree without the GIC describes another board. */
	if (info->gic_distributor.size == 0 || info->gic_cpu_interface.size == 0)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: both blocks, and only the enable bits to write. */
	*distributor = info->gic_distributor.base;
	*cpu_interface = info->gic_cpu_interface.base;
	*secure = 0;
	return HAL_OK;
}

/*
 * Reports the RAM the board keeps from the allocator: the loader's.
 */
unsigned
bsp_reserved_ranges(
	struct armv7_fdt_range *ranges,
	unsigned capacity)
{
	/* A caller with no room gets none. */
	if (capacity == 0)
		return 0;

	/* Succeeded: the loader's first 64 KiB of RAM. */
	ranges[0].base = ARMV7_PHYS_BASE;
	ranges[0].size = VIRT_LOADER_BYTES;
	return 1;
}

/*
 * Reports that the board is the emulator's.
 */
int
bsp_is_emulated(
	void)
{
	/* Succeeded: always emulated. */
	return 1;
}

/*
 * Resets the machine through the firmware; returns when it cannot.
 */
void
bsp_reset(
	void)
{
	/* PSCI's system reset. */
	virt_psci_system_reset();
}

/*
 * Turns the machine off through the firmware; returns when it cannot.
 */
void
bsp_poweroff(
	void)
{
	/* PSCI's system off. */
	virt_psci_system_off();
}

