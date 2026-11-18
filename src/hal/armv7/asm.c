/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 barriers, the memory-mapped I/O accessors, and the ways the
 * machine stops.
 *
 * Memory is ordered among the processors with dmb on the inner shareable
 * domain.  Device registers are ordered against memory with dsb, as Linux
 * does on ARMv7 (Procedure referenced: Linux v6.12
 * arch/arm/include/asm/io.h and barrier.h, __iormb and __iowmb): a read
 * is followed by one, so that memory the device wrote is read after it,
 * and a write is preceded by one of the stores, so that memory written for
 * the device is there before the device is told.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"

static void port_io_fatal(void);

/*
 * Orders every earlier memory access before every later one.
 */
void
hal_mb(
	void)
{
	/* Reads and writes, among the processors. */
	__asm__ volatile("dmb ish" : : : "memory");
}

/*
 * Orders earlier reads before later reads.
 */
void
hal_rmb(
	void)
{
	/* ARMv7 has no barrier for loads alone; the full one orders them. */
	__asm__ volatile("dmb ish" : : : "memory");
}

/*
 * Orders earlier writes before later writes.
 */
void
hal_wmb(
	void)
{
	/* Stores before, stores after, among the processors. */
	__asm__ volatile("dmb ishst" : : : "memory");
}

/*
 * Orders every earlier device and memory access before every later one.
 */
void
hal_io_mb(
	void)
{
	/* Completes everything before. */
	__asm__ volatile("dsb" : : : "memory");
}

/*
 * Orders earlier device and memory reads before later ones.
 */
void
hal_io_rmb(
	void)
{
	/* Completes everything before. */
	__asm__ volatile("dsb" : : : "memory");
}

/*
 * Orders earlier device and memory writes before later ones.
 */
void
hal_io_wmb(
	void)
{
	/* Completes the stores before. */
	__asm__ volatile("dsb st" : : : "memory");
}

/*
 * Reads one byte of a device register.
 */
uint8_t
hal_mmio_read8(
	const volatile void *addr)
{
	uint8_t value;

	/* The read, then the barrier that keeps later reads of memory after it. */
	value = *(const volatile uint8_t *)addr;
	__asm__ volatile("dsb" : : : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads two bytes of a device register.
 */
uint16_t
hal_mmio_read16(
	const volatile void *addr)
{
	uint16_t value;

	/* The read, then the barrier that keeps later reads of memory after it. */
	value = *(const volatile uint16_t *)addr;
	__asm__ volatile("dsb" : : : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads four bytes of a device register.
 */
uint32_t
hal_mmio_read32(
	const volatile void *addr)
{
	uint32_t value;

	/* The read, then the barrier that keeps later reads of memory after it. */
	value = *(const volatile uint32_t *)addr;
	__asm__ volatile("dsb" : : : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads eight bytes of a device register as two words, the low one first.
 */
uint64_t
hal_mmio_read64(
	const volatile void *addr)
{
	const volatile uint32_t *words;
	uint32_t low;
	uint32_t high;

	/* Two reads, then the barrier. */
	words = addr;
	low = words[0];
	high = words[1];
	__asm__ volatile("dsb" : : : "memory");

	/* Succeeded: the register's value. */
	return ((uint64_t)high << 32) | low;
}

/*
 * Writes one byte of a device register.
 */
void
hal_mmio_write8(
	volatile void *addr,
	uint8_t value)
{
	/* The barrier that completes earlier stores, then the write. */
	__asm__ volatile("dsb st" : : : "memory");
	*(volatile uint8_t *)addr = value;
}

/*
 * Writes two bytes of a device register.
 */
void
hal_mmio_write16(
	volatile void *addr,
	uint16_t value)
{
	/* The barrier that completes earlier stores, then the write. */
	__asm__ volatile("dsb st" : : : "memory");
	*(volatile uint16_t *)addr = value;
}

/*
 * Writes four bytes of a device register.
 */
void
hal_mmio_write32(
	volatile void *addr,
	uint32_t value)
{
	/* The barrier that completes earlier stores, then the write. */
	__asm__ volatile("dsb st" : : : "memory");
	*(volatile uint32_t *)addr = value;
}

/*
 * Writes eight bytes of a device register as two words, the low one first.
 */
void
hal_mmio_write64(
	volatile void *addr,
	uint64_t value)
{
	volatile uint32_t *words;

	/* The barrier, then the two writes. */
	words = addr;
	__asm__ volatile("dsb st" : : : "memory");
	words[0] = (uint32_t)value;
	words[1] = (uint32_t)(value >> 32);
}

/*
 * Refuses a port read: the architecture has no I/O ports.
 */
uint8_t
hal_io_inp8(
	uint16_t port)
{
	UNUSED_PARAMETER(port);

	/* A driver that uses ports does not belong on this port. */
	port_io_fatal();
	return 0;
}

/*
 * Refuses a port read: the architecture has no I/O ports.
 */
uint16_t
hal_io_inp16(
	uint16_t port)
{
	UNUSED_PARAMETER(port);

	/* A driver that uses ports does not belong on this port. */
	port_io_fatal();
	return 0;
}

/*
 * Refuses a port read: the architecture has no I/O ports.
 */
uint32_t
hal_io_inp32(
	uint16_t port)
{
	UNUSED_PARAMETER(port);

	/* A driver that uses ports does not belong on this port. */
	port_io_fatal();
	return 0;
}

/*
 * Refuses a port write: the architecture has no I/O ports.
 */
void
hal_io_outp8(
	uint16_t port,
	uint8_t value)
{
	UNUSED_PARAMETER(port);
	UNUSED_PARAMETER(value);

	/* A driver that uses ports does not belong on this port. */
	port_io_fatal();
}

/*
 * Refuses a port write: the architecture has no I/O ports.
 */
void
hal_io_outp16(
	uint16_t port,
	uint16_t value)
{
	UNUSED_PARAMETER(port);
	UNUSED_PARAMETER(value);

	/* A driver that uses ports does not belong on this port. */
	port_io_fatal();
}

/*
 * Refuses a port write: the architecture has no I/O ports.
 */
void
hal_io_outp32(
	uint16_t port,
	uint32_t value)
{
	UNUSED_PARAMETER(port);
	UNUSED_PARAMETER(value);

	/* A driver that uses ports does not belong on this port. */
	port_io_fatal();
}

/*
 * Stops the calling processor for good, with interrupts masked.
 */
void
armv7_halt_forever(
	void)
{
	/* Nothing wakes the processor into the kernel again. */
	__asm__ volatile("cpsid if" : : : "memory");

	/* Waits for ever; a stray wake waits again. */
	for (;;)
		armv7_wfi();
}

/*
 * Resets the system for a reboot.
 */
void
hal_reset(
	void)
{
	/* The board's reset; a board that cannot reset halts instead. */
	bsp_reset();
	armv7_halt_forever();
}

/*
 * Turns the system off.
 */
void
hal_poweroff(
	void)
{
	/* The board's power off; a board that cannot halts instead. */
	bsp_poweroff();
	armv7_halt_forever();
}

/*
 * Stops the calling processor for good after a kernel panic.
 */
void
hal_panic(
	void)
{
	/* The panic was already reported. */
	armv7_halt_forever();
}

/* Stops the kernel at a port access. */
static void
port_io_fatal(
	void)
{
	/* Port I/O has no meaning on this architecture. */
	HAL_FATAL("port I/O is unavailable on armv7");
}
