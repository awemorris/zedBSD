/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's PCI I/O ports and memory-mapped device accessors.
 *
 * A port number is an offset into the PCI I/O space, which the HAL reaches
 * through the physical bypass in little-endian order, the order of the PCI
 * bus; no mapping is needed.  A memory-mapped device is reached through the
 * kernel address hal_space_map_device() gave, whose pages invert the byte
 * order, so plain loads and stores read the device's little-endian
 * registers as numbers.
 */

#include <hal/hal.h>
#include "asi.h"
#include "io.h"

/*
 * The physical address of the PCI I/O space's port zero.
 *
 * Set once by sparcv9_io_init() before any driver runs and constant
 * afterwards; zero until then.
 */
static uint64_t pci_io_base;

/*
 * Records where the PCI I/O space lies.
 */
void
sparcv9_io_init(
	uint64_t base)
{
	/* Keeps the base for every later port access. */
	pci_io_base = base;
}

/*
 * Reports the physical address of a PCI I/O port.
 */
uint64_t
sparcv9_io_port_address(
	uint16_t port)
{
	/* Succeeded: the port's place in the I/O space. */
	return pci_io_base + port;
}

/*
 * Reads one byte from a PCI I/O port.
 */
uint8_t
hal_io_inp8(
	uint16_t port)
{
	uint8_t value;

	/* Loads the byte past the caches. */
	value = sparcv9_phys_read8(pci_io_base + port);

	/* Succeeded: the byte. */
	return value;
}

/*
 * Reads a 16-bit word from a PCI I/O port.
 */
uint16_t
hal_io_inp16(
	uint16_t port)
{
	uint16_t value;

	/* Loads the little-endian word past the caches. */
	value = sparcv9_phys_read16_le(pci_io_base + port);

	/* Succeeded: the word. */
	return value;
}

/*
 * Reads a 32-bit word from a PCI I/O port.
 */
uint32_t
hal_io_inp32(
	uint16_t port)
{
	uint32_t value;

	/* Loads the little-endian word past the caches. */
	value = sparcv9_phys_read32_le(pci_io_base + port);

	/* Succeeded: the word. */
	return value;
}

/*
 * Writes one byte to a PCI I/O port.
 */
void
hal_io_outp8(
	uint16_t port,
	uint8_t value)
{
	/* Stores the byte past the caches. */
	sparcv9_phys_write8(pci_io_base + port, value);
}

/*
 * Writes a 16-bit word to a PCI I/O port.
 */
void
hal_io_outp16(
	uint16_t port,
	uint16_t value)
{
	/* Stores the little-endian word past the caches. */
	sparcv9_phys_write16_le(pci_io_base + port, value);
}

/*
 * Writes a 32-bit word to a PCI I/O port.
 */
void
hal_io_outp32(
	uint16_t port,
	uint32_t value)
{
	/* Stores the little-endian word past the caches. */
	sparcv9_phys_write32_le(pci_io_base + port, value);
}

/*
 * Reads one byte of a mapped device.
 */
uint8_t
hal_mmio_read8(
	const volatile void *address)
{
	uint8_t value;

	/* Loads through the device mapping. */
	value = *(const volatile uint8_t *)address;

	/* Succeeded: the byte. */
	return value;
}

/*
 * Reads a 16-bit register of a mapped device.
 */
uint16_t
hal_mmio_read16(
	const volatile void *address)
{
	uint16_t value;

	/* Loads through the device mapping, which orders the bytes. */
	value = *(const volatile uint16_t *)address;

	/* Succeeded: the register. */
	return value;
}

/*
 * Reads a 32-bit register of a mapped device.
 */
uint32_t
hal_mmio_read32(
	const volatile void *address)
{
	uint32_t value;

	/* Loads through the device mapping, which orders the bytes. */
	value = *(const volatile uint32_t *)address;

	/* Succeeded: the register. */
	return value;
}

/*
 * Reads a 64-bit register of a mapped device.
 */
uint64_t
hal_mmio_read64(
	const volatile void *address)
{
	uint64_t value;

	/* Loads through the device mapping, which orders the bytes. */
	value = *(const volatile uint64_t *)address;

	/* Succeeded: the register. */
	return value;
}

/*
 * Writes one byte of a mapped device.
 */
void
hal_mmio_write8(
	volatile void *address,
	uint8_t value)
{
	/* Stores through the device mapping. */
	*(volatile uint8_t *)address = value;
}

/*
 * Writes a 16-bit register of a mapped device.
 */
void
hal_mmio_write16(
	volatile void *address,
	uint16_t value)
{
	/* Stores through the device mapping, which orders the bytes. */
	*(volatile uint16_t *)address = value;
}

/*
 * Writes a 32-bit register of a mapped device.
 */
void
hal_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	/* Stores through the device mapping, which orders the bytes. */
	*(volatile uint32_t *)address = value;
}

/*
 * Writes a 64-bit register of a mapped device.
 */
void
hal_mmio_write64(
	volatile void *address,
	uint64_t value)
{
	/* Stores through the device mapping, which orders the bytes. */
	*(volatile uint64_t *)address = value;
}
