/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's three system control blocks.
 *
 * Each block is a page of registers that configure parts of other devices:
 * the SYS block holds the PLL settings and the SD controllers' sample
 * phase, the STG block the PCIe controllers' bus settings, and the AON
 * block the Ethernet controller's interface mode.  Several drivers share
 * a register, so every change is a read-modify-write under one lock.
 */

#ifndef KERN_DRIVERS_VF2_SYSCON_H
#define KERN_DRIVERS_VF2_SYSCON_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * One of the three blocks.
 *
 * The numbers index the blocks' records; COUNT is not a block.
 */
enum drv_vf2_syscon {
	DRV_VF2_SYSCON_SYS = 0,
	DRV_VF2_SYSCON_STG = 1,
	DRV_VF2_SYSCON_AON = 2,
	DRV_VF2_SYSCON_COUNT = 3
};

/*
 * Finds the three blocks in the device tree and maps them.
 *
 * Touches no register.  A block the tree lacks stays absent and its calls
 * report ENODEV; the call reports ENODEV only when the SYS block, which
 * holds the PLL settings, is missing.
 */
int drv_vf2_syscon_init(const struct drv_fdt *fdt);

/*
 * Reads one register of a block.
 *
 * The offset is a multiple of four inside the block.
 */
int drv_vf2_syscon_read(enum drv_vf2_syscon syscon, uint32_t offset, uint32_t *value);

/*
 * Changes the bits of one register that the mask selects.
 *
 * The value's bits outside the mask are ignored.  The register is written
 * only when the masked bits differ from what it holds.
 */
int drv_vf2_syscon_update(enum drv_vf2_syscon syscon, uint32_t offset, uint32_t mask, uint32_t value);

/*
 * Reads a property that names a block by phandle, followed by arguments.
 *
 * Properties such as starfive,syscon and starfive,sysreg have this shape:
 * the phandle of a block, then the cells the binding defines.  Up to
 * capacity cells after the phandle are copied to arguments and count says
 * how many there were.  ENOENT means the node lacks the property; ENODEV
 * that the phandle names no block found by drv_vf2_syscon_init().
 */
int drv_vf2_syscon_lookup(const struct drv_fdt *fdt, uint32_t node, const char *name, enum drv_vf2_syscon *syscon, uint32_t *arguments, unsigned capacity, unsigned *count);

#endif
