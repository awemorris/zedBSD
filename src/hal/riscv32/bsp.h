/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the riscv32 HAL asks of its board: the parts of a machine that the
 * device tree does not describe.
 */

#ifndef KERN_HAL_RISCV32_BSP_H
#define KERN_HAL_RISCV32_BSP_H

#include <hal/types.h>
#include "fdt.h"

void riscv32_board_init(const struct riscv32_fdt_info *info);
void riscv32_board_putc(int character);
void riscv32_board_describe(void);

#endif
