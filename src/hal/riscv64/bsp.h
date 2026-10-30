/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the riscv64 HAL asks of its board: the parts of a machine that the
 * device tree does not describe.
 */

#ifndef KERN_HAL_RISCV64_BSP_H
#define KERN_HAL_RISCV64_BSP_H

#include <hal/types.h>
#include "fdt.h"

void
riscv64_board_init(
	const struct riscv64_fdt_info *info);

void *
riscv64_board_handoff(
	const char *name);

void
riscv64_board_describe(
	void);

/*
 * The distance from RAM to the board's uncached alias of it, or zero when
 * the board has no alias it can keep coherent with the caches.
 */
uint64_t
riscv64_board_uncached_offset(
	void);

/*
 * Writes back and discards every cached line of a physical range; nothing
 * on a board whose devices snoop the caches.
 */
void
riscv64_board_flush_physical(
	uint64_t physical,
	uint64_t size);

/*
 * Reports the screen the board's early console draws on and names to the
 * kernel: 1 with its physical base and size, or 0 for none.  The kernel's
 * tables give the console its own window of it, which the kernel's
 * console takes over and removes.
 */
int
riscv64_board_screen(
	uint64_t *physical,
	uint64_t *size);

/*
 * Has the early console draw the screen through the window from now on,
 * address being where the window holds the screen's first byte.  Called on
 * the boot hart, before any other hart runs, once the kernel's tables are
 * in use.
 */
void
riscv64_board_screen_window(
	uintptr_t address);

#endif
