/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 barriers, the memory-mapped I/O accessors, and the ways the
 * machine stops.
 *
 * Device registers are ordered against memory with fences on the I/O and
 * memory sets: a read is followed by a fence that keeps later accesses
 * after it, and a write is preceded by one that keeps earlier stores
 * before it.
 */

#include <hal/hal.h>
#include "asm.h"
#include "sbi.h"

static void port_io_fatal(void);

/*
 * Orders every earlier memory access before every later one.
 */
void
hal_mb(
	void)
{
	/* Reads and writes before, reads and writes after. */
	__asm__ volatile("fence rw, rw" : : : "memory");
}

/*
 * Orders earlier reads before later reads.
 */
void
hal_rmb(
	void)
{
	/* Reads before, reads after. */
	__asm__ volatile("fence r, r" : : : "memory");
}

/*
 * Orders earlier writes before later writes.
 */
void
hal_wmb(
	void)
{
	/* Writes before, writes after. */
	__asm__ volatile("fence w, w" : : : "memory");
}

/*
 * Orders every earlier device and memory access before every later one.
 */
void
hal_io_mb(
	void)
{
	/* Device and memory reads and writes, both ways. */
	__asm__ volatile("fence iorw, iorw" : : : "memory");
}

/*
 * Orders earlier device and memory reads before later ones.
 */
void
hal_io_rmb(
	void)
{
	/* Reads of devices and memory before, reads after. */
	__asm__ volatile("fence ir, ir" : : : "memory");
}

/*
 * Orders earlier device and memory writes before later ones.
 */
void
hal_io_wmb(
	void)
{
	/* Writes of devices and memory before, writes after. */
	__asm__ volatile("fence ow, ow" : : : "memory");
}

/*
 * Reads one byte of a device register.
 */
uint8_t
hal_mmio_read8(
	const volatile void *addr)
{
	uint8_t value;

	/* The read, then the fence that keeps later accesses after it. */
	value = *(const volatile uint8_t *)addr;
	__asm__ volatile("fence i, ir" : : : "memory");

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

	/* The read, then the fence that keeps later accesses after it. */
	value = *(const volatile uint16_t *)addr;
	__asm__ volatile("fence i, ir" : : : "memory");

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

	/* The read, then the fence that keeps later accesses after it. */
	value = *(const volatile uint32_t *)addr;
	__asm__ volatile("fence i, ir" : : : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads eight bytes of a device register.
 */
uint64_t
hal_mmio_read64(
	const volatile void *addr)
{
	uint64_t value;

	/* The read, then the fence that keeps later accesses after it. */
	value = *(const volatile uint64_t *)addr;
	__asm__ volatile("fence i, ir" : : : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes one byte of a device register.
 */
void
hal_mmio_write8(
	volatile void *addr,
	uint8_t value)
{
	/* The fence that keeps earlier stores before it, then the write. */
	__asm__ volatile("fence w, o" : : : "memory");
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
	/* The fence that keeps earlier stores before it, then the write. */
	__asm__ volatile("fence w, o" : : : "memory");
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
	/* The fence that keeps earlier stores before it, then the write. */
	__asm__ volatile("fence w, o" : : : "memory");
	*(volatile uint32_t *)addr = value;
}

/*
 * Writes eight bytes of a device register.
 */
void
hal_mmio_write64(
	volatile void *addr,
	uint64_t value)
{
	/* The fence that keeps earlier stores before it, then the write. */
	__asm__ volatile("fence w, o" : : : "memory");
	*(volatile uint64_t *)addr = value;
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

	/* A value for the compiler; the stop above does not return. */
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

	/* A value for the compiler; the stop above does not return. */
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

	/* A value for the compiler; the stop above does not return. */
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
 * Stops the calling hart for good, with interrupts off.
 */
void
riscv64_halt_forever(
	void)
{
	/* Nothing wakes the hart into the kernel again. */
	(void)riscv64_irq_save();
	riscv64_clear_sie(RISCV64_SIE_SSIE | RISCV64_SIE_STIE | RISCV64_SIE_SEIE);

	/* Waits for ever; a stray wake waits again. */
	for (;;)
		riscv64_wfi();
}

/*
 * Resets the system for a reboot.
 */
void
hal_reset(
	void)
{
	/* The firmware's cold reset, or a halt when it has none. */
	riscv64_sbi_reset(1);
}

/*
 * Turns the system off.
 */
void
hal_poweroff(
	void)
{
	/* The firmware's shutdown, or a halt when it has none. */
	riscv64_sbi_shutdown();
}

/*
 * Stops the calling hart for good after a kernel panic.
 */
void
hal_panic(
	void)
{
	/* The panic was already reported. */
	riscv64_halt_forever();
}

/* Stops the kernel at a port access. */
static void
port_io_fatal(
	void)
{
	/* Port I/O has no meaning on this architecture. */
	HAL_FATAL("port I/O is unavailable on riscv64");
}
