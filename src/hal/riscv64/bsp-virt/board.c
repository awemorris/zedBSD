/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The emulator's virt board.
 *
 * Everything the kernel needs is in the device tree but the display: the
 * board shows a RAM framebuffer, which the HAL sets up through the
 * firmware configuration interface and draws its early console on.  The
 * board's devices snoop the caches, so cache maintenance for them is
 * nothing at all.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../defs.h"
#include "fwcfg.h"
#include "framebuffer.h"

/*
 * Why the display could not be shown, or HAL_OK when it is.  Kept for the
 * boot log; set once on the boot hart.
 */
static int board_display_error = HAL_ERR_UNSUPPORTED;

/*
 * Sets up the board's display and draws the early console's lines on it.
 *
 * A machine without the configuration interface or without the
 * framebuffer device boots on the firmware's console alone.
 */
void
riscv64_board_init(
	const struct riscv64_fdt_info *info)
{
	int error;

	/* Finds the configuration interface the device tree names. */
	error = virt_fwcfg_init(info->fwcfg_base, info->fwcfg_size);
	if (error != HAL_OK) {
		board_display_error = error;
		return;
	}

	/* Shows the screen's memory and draws what the console printed before it. */
	error = virt_framebuffer_init();
	board_display_error = error;
}

/*
 * Answers a handoff name the common names do not cover; the board has
 * none of its own.
 *
 * The screen is the hardware layer's RAM framebuffer, not one the
 * firmware left on, so the board names no framebuffer to the kernel even
 * when a loader handed one over.
 */
void *
riscv64_board_handoff(
	const char *name)
{
	UNUSED_PARAMETER(name);

	/* An unknown handoff. */
	return NULL;
}

/*
 * Reports no screen the kernel can take over: the board's screen is the
 * hardware layer's own RAM framebuffer.
 */
int
riscv64_board_screen(
	uint64_t *physical,
	uint64_t *size)
{
	UNUSED_PARAMETER(physical);
	UNUSED_PARAMETER(size);

	/* No screen is named to the kernel. */
	return 0;
}

/*
 * Ignores the early console's window of a screen: the board has none.
 */
void
riscv64_board_screen_window(
	uintptr_t address)
{
	UNUSED_PARAMETER(address);
}

/*
 * Writes the board's display to the boot log.
 */
void
riscv64_board_describe(
	void)
{
	/* Describes the screen, or why there is none. */
	if (board_display_error == HAL_OK) {
		virt_framebuffer_describe();
		return;
	}

	/* Says why there is no screen. */
	hal_printf("RISCV64 FRAMEBUFFER unavailable (%u); firmware console only\n",
		   (uint32_t)board_display_error);
}

/*
 * Reports that the board has no uncached alias of RAM: its devices snoop
 * the caches, so nothing needs one.
 */
uint64_t
riscv64_board_uncached_offset(
	void)
{
	/* No alias exists. */
	return 0;
}

/*
 * Writes back and discards the lines of a physical range: nothing, as the
 * board's devices snoop the caches.
 */
void
riscv64_board_flush_physical(
	uint64_t physical,
	uint64_t size)
{
	UNUSED_PARAMETER(physical);
	UNUSED_PARAMETER(size);
}

/*
 * Writes cached lines of a range back to memory; the board's devices
 * snoop the caches, so there is nothing to do.
 */
void
hal_dcache_clean_range(
	uintptr_t address,
	size_t size)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);
}

/*
 * Discards cached lines of a range; nothing to do on a coherent board.
 */
void
hal_dcache_invalidate_range(
	uintptr_t address,
	size_t size)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);
}

/*
 * Writes back and discards cached lines of a range; nothing to do on a
 * coherent board.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t address,
	size_t size)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);
}
