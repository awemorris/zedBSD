/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The x86 port I/O implementation.
 *
 * The instructions are identical on i386 and amd64, so both boards share
 * this file. Drivers reach these through kern_io_in8() and its siblings
 * rather than calling the HAL directly.
 */

#include <hal/hal.h>

/*
 * Reads one byte from an I/O port.
 */
uint8_t
hal_io_inp8(
	uint16_t port)
{
	uint8_t value;

	/* Issues the byte input instruction. */
	__asm__ volatile("inb %w1, %0" : "=a"(value) : "Nd"(port) : "memory");

	/* Returns the byte the device supplied. */
	return value;
}

/*
 * Reads two bytes from an I/O port.
 */
uint16_t
hal_io_inp16(
	uint16_t port)
{
	uint16_t value;

	/* Issues the word input instruction. */
	__asm__ volatile("inw %w1, %0" : "=a"(value) : "Nd"(port) : "memory");

	/* Returns the word the device supplied. */
	return value;
}

/*
 * Reads four bytes from an I/O port.
 */
uint32_t
hal_io_inp32(
	uint16_t port)
{
	uint32_t value;

	/* Issues the doubleword input instruction. */
	__asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(port) : "memory");

	/* Returns the doubleword the device supplied. */
	return value;
}

/*
 * Writes one byte to an I/O port.
 */
void
hal_io_outp8(
	uint16_t port,
	uint8_t value)
{
	/* Issues the byte output instruction. */
	__asm__ volatile("outb %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}

/*
 * Writes two bytes to an I/O port.
 */
void
hal_io_outp16(
	uint16_t port,
	uint16_t value)
{
	/* Issues the word output instruction. */
	__asm__ volatile("outw %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}

/*
 * Writes four bytes to an I/O port.
 */
void
hal_io_outp32(
	uint16_t port,
	uint32_t value)
{
	/* Issues the doubleword output instruction. */
	__asm__ volatile("outl %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}
