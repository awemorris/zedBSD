/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 early text console.
 */

#ifndef KERN_HAL_RISCV64_CONS_H
#define KERN_HAL_RISCV64_CONS_H

void
riscv64_cons_putc(
	int character);

void
riscv64_cons_redraw(
	void);

void
riscv64_cons_detach_screen(
	void);

#endif
