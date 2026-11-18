/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's early text console on the framebuffer.
 */

#ifndef KERN_HAL_RISCV32_BSP_VIRT_CONS_H
#define KERN_HAL_RISCV32_BSP_VIRT_CONS_H

void
virt_cons_putc(
	int character);

void
virt_cons_attach(
	void);

#endif
