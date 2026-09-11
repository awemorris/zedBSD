/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device register access and ordering barriers for drivers.
 *
 * Each call forwards to the HAL. The indirection exists so that a driver
 * never names a HAL symbol: the layer boundary is what makes the porting
 * contract meaningful.
 */

#include <hal/hal.h>

#include "kern/device-io.h"

/* Reads one byte from an I/O port. */
uint8_t
kern_io_in8(
	uint16_t port)
{
	return hal_io_inp8(port);
}

/* Reads two bytes from an I/O port. */
uint16_t
kern_io_in16(
	uint16_t port)
{
	return hal_io_inp16(port);
}

/* Reads four bytes from an I/O port. */
uint32_t
kern_io_in32(
	uint16_t port)
{
	return hal_io_inp32(port);
}

/* Writes one byte to an I/O port. */
void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	hal_io_outp8(port, value);
}

/* Writes two bytes to an I/O port. */
void
kern_io_out16(
	uint16_t port,
	uint16_t value)
{
	hal_io_outp16(port, value);
}

/* Writes four bytes to an I/O port. */
void
kern_io_out32(
	uint16_t port,
	uint32_t value)
{
	hal_io_outp32(port, value);
}

/* Reads one byte from a mapped device register. */
uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	return hal_mmio_read8(address);
}

/* Reads two bytes from a mapped device register. */
uint16_t
kern_mmio_read16(
	const volatile void *address)
{
	return hal_mmio_read16(address);
}

/* Reads four bytes from a mapped device register. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	return hal_mmio_read32(address);
}

/* Reads eight bytes from a mapped device register. */
uint64_t
kern_mmio_read64(
	const volatile void *address)
{
	return hal_mmio_read64(address);
}

/* Writes one byte to a mapped device register. */
void
kern_mmio_write8(
	volatile void *address,
	uint8_t value)
{
	hal_mmio_write8(address, value);
}

/* Writes two bytes to a mapped device register. */
void
kern_mmio_write16(
	volatile void *address,
	uint16_t value)
{
	hal_mmio_write16(address, value);
}

/* Writes four bytes to a mapped device register. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	hal_mmio_write32(address, value);
}

/* Writes eight bytes to a mapped device register. */
void
kern_mmio_write64(
	volatile void *address,
	uint64_t value)
{
	hal_mmio_write64(address, value);
}

/* Orders device reads and writes against each other. */
void
kern_io_barrier(
	void)
{
	hal_io_mb();
}

/* Orders device reads against each other. */
void
kern_io_read_barrier(
	void)
{
	hal_io_rmb();
}

/* Orders device writes against each other. */
void
kern_io_write_barrier(
	void)
{
	hal_io_wmb();
}

/* Stops the compiler from moving accesses across this point. */
void
kern_compiler_barrier(
	void)
{
	hal_compiler_barrier();
}
