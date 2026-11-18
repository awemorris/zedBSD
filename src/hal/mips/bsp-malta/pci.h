/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's PCI configuration space, through the system
 * controller's configuration registers.
 */

#ifndef KERN_HAL_MIPS_BSP_MALTA_PCI_H
#define KERN_HAL_MIPS_BSP_MALTA_PCI_H

#include <hal/types.h>

/* The devices of bus 0 the HAL looks at; device 0 is the system controller. */
#define MALTA_PCI_DEVICES	32U

/*
 * The byte offsets in a device's configuration header the HAL uses: the
 * identity, the command register and the first base address register.
 *
 * Values referenced: PCI Local Bus Specification 3.0, the type 0
 * configuration header (vendor and device at 0x00, command at 0x04, base
 * address registers from 0x10, four bytes each; the memory space enable
 * bit 1 of the command register; bit 0 of a base address register set
 * for an I/O range, bits 0-3 the memory type).
 */
#define MALTA_PCI_IDENTITY	0x00U
#define MALTA_PCI_COMMAND	0x04U
#define MALTA_PCI_BAR0		0x10U
#define MALTA_PCI_COMMAND_MEMORY	0x0002U
#define MALTA_PCI_BAR_IO	0x00000001U
#define MALTA_PCI_BAR_MEMORY_FLAGS	0x0000000fU

uint32_t
malta_pci_read(
	unsigned device,
	unsigned offset);

void
malta_pci_write(
	unsigned device,
	unsigned offset,
	uint32_t value);

int
malta_pci_find(
	uint16_t vendor,
	uint16_t product,
	unsigned *device);

uint32_t
malta_pci_bar_size(
	unsigned device,
	unsigned bar);

#endif
