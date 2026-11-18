/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The emulator's firmware configuration interface, as the virt board's
 * HAL uses it: finding a file and writing it.
 */

#ifndef KERN_HAL_RISCV64_BSP_VIRT_FWCFG_H
#define KERN_HAL_RISCV64_BSP_VIRT_FWCFG_H

#include <hal/types.h>

int
virt_fwcfg_init(
	uint64_t base_phys,
	uint64_t size);

int
virt_fwcfg_find(
	const char *name,
	uint16_t *selector,
	uint32_t *size);

int
virt_fwcfg_write(
	uint16_t selector,
	const void *data,
	uint32_t length);

#endif
