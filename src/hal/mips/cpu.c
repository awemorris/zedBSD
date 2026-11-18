/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS processor's idle and halt, the barriers, the memory-mapped I/O
 * accessors and the stop after a panic.
 *
 * sync orders every access, device accesses through the uncached KSEG1
 * included.  The port is uniprocessor: the processor counts and
 * notifications describe one processor.
 */

#include <hal/hal.h>
#include "asm.h"
#include "cpu.h"
#include "spin.h"

static void port_io_fatal(void);

/*
 * Waits for an interrupt, takes it, and leaves with interrupts off.
 */
void
hal_cpu_idle(
	void)
{
	/*
	 * Interrupts on, the wait, then off again.  An interrupt taken
	 * between the first two only makes the wait last to the next one,
	 * at most a tick.
	 */
	__asm__ volatile("ei\n\tehb\n\twait\n\tdi\n\tehb" : : : "memory");
}

/*
 * Waits until the next interrupt when interrupts are on.
 */
void
hal_halt(
	void)
{
	uint32_t status;

	/* With interrupts off nothing can wake it, so it only spins once. */
	status = mips_read_status();
	if ((status & MIPS_STATUS_INTERRUPTS) == 0) {
		__asm__ volatile("" : : : "memory");
		return;
	}

	/* Waits for the next interrupt with interrupts on. */
	__asm__ volatile("wait" : : : "memory");
}

/*
 * Orders every earlier memory access before every later one.
 */
void
hal_mb(
	void)
{
	/* Reads and writes before, reads and writes after. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Orders earlier reads before later reads.
 */
void
hal_rmb(
	void)
{
	/* Reads before, reads after. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Orders earlier writes before later writes.
 */
void
hal_wmb(
	void)
{
	/* Writes before, writes after. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Orders every earlier device and memory access before every later one.
 */
void
hal_io_mb(
	void)
{
	/* Device and memory reads and writes, both ways. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Orders earlier device and memory reads before later ones.
 */
void
hal_io_rmb(
	void)
{
	/* Reads of devices and memory before, reads after. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Orders earlier device and memory writes before later ones.
 */
void
hal_io_wmb(
	void)
{
	/* Writes of devices and memory before, writes after. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Reads one byte of a device register.
 */
uint8_t
hal_mmio_read8(
	const volatile void *addr)
{
	uint8_t value;

	/* The read, then the pipeline's synchronization. */
	value = *(const volatile uint8_t *)addr;
	__asm__ volatile("sync" : : : "memory");

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

	/* The read, then the pipeline's synchronization. */
	value = *(const volatile uint16_t *)addr;
	__asm__ volatile("sync" : : : "memory");

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

	/* The read, then the pipeline's synchronization. */
	value = *(const volatile uint32_t *)addr;
	__asm__ volatile("sync" : : : "memory");

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

	/* The read, then the pipeline's synchronization. */
	value = *(const volatile uint64_t *)addr;
	__asm__ volatile("sync" : : : "memory");

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
	/* The pipeline's synchronization, then the write. */
	__asm__ volatile("sync" : : : "memory");
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
	/* The pipeline's synchronization, then the write. */
	__asm__ volatile("sync" : : : "memory");
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
	/* The pipeline's synchronization, then the write. */
	__asm__ volatile("sync" : : : "memory");
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
	/* The pipeline's synchronization, then the write. */
	__asm__ volatile("sync" : : : "memory");
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
 * Stops the calling processor for good after a kernel panic.
 */
void
hal_panic(
	void)
{
	/* The panic was already reported. */
	mips_halt_forever();
}

/*
 * Reports the calling processor: there is one.
 */
hal_cpu_id_t
hal_cpu_current(
	void)
{
	/* The only processor. */
	return 0;
}

/*
 * Reports how many processors the HAL brings up.
 */
unsigned
hal_cpu_count(
	void)
{
	/* The port is uniprocessor. */
	return 1;
}

/*
 * Reports the processors that are up: processor 0.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	/* A caller with nowhere to put the answer asked for nothing. */
	if (result == NULL)
		return;
	hal_cpu_mask_zero(result);
	hal_cpu_mask_set(result, 0);
}

/*
 * Refuses a notification: there is no other processor to notify.
 */
int
hal_cpu_notify(
	hal_cpu_id_t cpu)
{
	/* Processor 0 is the caller itself; any other does not exist. */
	if (cpu == 0)
		return HAL_ERR_UNSUPPORTED;
	return HAL_ERR_INVALID;
}

/*
 * Refuses a notification to a set of processors.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *targets)
{
	hal_cpu_id_t cpu;
	bool member;

	/* The set must hold processor 0. */
	if (targets == NULL)
		return HAL_ERR_INVALID;
	member = hal_cpu_mask_test(targets, 0);
	if (!member)
		return HAL_ERR_INVALID;

	/* The set may hold no other processor. */
	for (cpu = 1; cpu < HAL_CPU_MAX; cpu++) {
		member = hal_cpu_mask_test(targets, cpu);
		if (member)
			return HAL_ERR_INVALID;
	}

	/* Failed: notifying itself is not offered. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Stops the processor for good.
 */
_Noreturn void
hal_cpu_park(
	void)
{
	/* The mask stays closed. */
	mips_halt_forever();
}

/*
 * Stops every processor after a panic: this one.
 */
_Noreturn void
hal_cpu_panic_all(
	void)
{
	/* The same stop. */
	mips_halt_forever();
}

/* Stops the kernel at a port access. */
static void
port_io_fatal(
	void)
{
	/* Port I/O has no meaning on this architecture. */
	HAL_FATAL("port I/O is unavailable on mips");
}
