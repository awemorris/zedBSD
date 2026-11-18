/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's PCI configuration space.
 *
 * The board's system controller, a GT-64120, holds the configuration
 * address and data registers of its PCI bus in the PC's form: a word that
 * names the bus, device, function and register is written to the address
 * register, and the register is then read or written through the data
 * register.  The HAL reaches only bus 0 and function 0, which is where
 * the devices of the emulated board are; it never uses the controller's
 * other registers.
 */

#include <hal/hal.h>
#include "../io.h"
#include "pci.h"

/*
 * The physical addresses of the configuration address and data
 * registers: the controller's registers at the base the loader's
 * bootloader moves them to, at offsets 0xcf8 and 0xcfc.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (write_bootloader, the
 * system controller's register base 0x1be00000) and
 * hw/pci-host/gt64120.c (GT_PCI0_CFGADDR, GT_PCI0_CFGDATA).
 */
#define PCI_CONFIG_ADDRESS	0x1be00cf8U
#define PCI_CONFIG_DATA		0x1be00cfcU

/*
 * The fields of a configuration address word: the enable bit, and the
 * places of the device number and of the register's word offset.
 *
 * Values referenced: PCI Local Bus Specification 3.0, configuration
 * mechanism #1 (enable bit 31, bus bits 16-23, device bits 11-15,
 * function bits 8-10, register bits 2-7).
 */
#define PCI_CONFIG_ENABLE	0x80000000U
#define PCI_CONFIG_DEVICE_SHIFT	11U
#define PCI_CONFIG_REGISTER_MASK	0xfcU

/* The identity a slot reads as when no device answers. */
#define PCI_NO_DEVICE		0xffffffffU

static void select_register(unsigned device, unsigned offset);

/*
 * Reads a 32-bit register of a device's configuration header on bus 0.
 */
uint32_t
malta_pci_read(
	unsigned device,
	unsigned offset)
{
	uint32_t value;

	/* Names the register, then reads it through the data register. */
	select_register(device, offset);
	value = mips_io_read32(PCI_CONFIG_DATA);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a 32-bit register of a device's configuration header on bus 0.
 */
void
malta_pci_write(
	unsigned device,
	unsigned offset,
	uint32_t value)
{
	/* Names the register, then writes it through the data register. */
	select_register(device, offset);
	mips_io_write32(PCI_CONFIG_DATA, value);
}

/*
 * Finds the first device on bus 0 with a vendor and product.
 *
 * Device 0, the system controller itself, is passed over.  Reports
 * HAL_OK and the device's number, or HAL_ERR_UNSUPPORTED.
 */
int
malta_pci_find(
	uint16_t vendor,
	uint16_t product,
	unsigned *device)
{
	uint32_t wanted;
	uint32_t identity;
	unsigned candidate;

	/* The identity word as the header holds it: the product above the vendor. */
	wanted = ((uint32_t)product << 16) | (uint32_t)vendor;

	/* Reads each slot's identity in order, from the first after the controller. */
	for (candidate = 1; candidate < MALTA_PCI_DEVICES; candidate++) {
		identity = malta_pci_read(candidate, MALTA_PCI_IDENTITY);
		if (identity == PCI_NO_DEVICE)
			continue;
		if (identity != wanted)
			continue;

		/* Succeeded: the device found. */
		*device = candidate;
		return HAL_OK;
	}

	/* Failed: no device of that identity. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Measures the memory range a base address register decodes.
 *
 * All ones are written to the register and read back; the bits the
 * device keeps at zero are the range's size.  The register's old value is
 * written back.  Reports zero for an I/O range or a register without a
 * range.
 */
uint32_t
malta_pci_bar_size(
	unsigned device,
	unsigned bar)
{
	unsigned offset;
	uint32_t old_value;
	uint32_t probe;
	uint32_t mask;

	/* Probes the register with all ones, and puts the old value back. */
	offset = MALTA_PCI_BAR0 + bar * 4U;
	old_value = malta_pci_read(device, offset);
	malta_pci_write(device, offset, 0xffffffffU);
	probe = malta_pci_read(device, offset);
	malta_pci_write(device, offset, old_value);

	/* An I/O range, or a register the device does not implement, has no memory size. */
	if ((probe & MALTA_PCI_BAR_IO) != 0)
		return 0;
	mask = probe & ~MALTA_PCI_BAR_MEMORY_FLAGS;
	if (mask == 0)
		return 0;

	/* Succeeded: the size is the lowest address bit the device decodes. */
	return ~mask + 1U;
}

/* Writes the configuration address that names a register of a device on bus 0. */
static void
select_register(
	unsigned device,
	unsigned offset)
{
	uint32_t address;

	/* Bus 0, function 0: only the device and the register's word offset vary. */
	address = PCI_CONFIG_ENABLE;
	address |= (uint32_t)device << PCI_CONFIG_DEVICE_SHIFT;
	address |= (uint32_t)offset & PCI_CONFIG_REGISTER_MASK;
	mips_io_write32(PCI_CONFIG_ADDRESS, address);
}
