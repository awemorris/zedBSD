/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC processor record, the idle and halt, the data
 * cache's maintenance, the barriers and the memory-mapped I/O accessors.
 *
 * sync orders every access; eieio orders device accesses, which the
 * uncached, guarded windows do not reorder further.
 */

#include <hal/hal.h>
#include "asm.h"
#include "board.h"
#include "percpu.h"

/* The one processor's record. */
static struct ppc64_percpu boot_cpu;

static void port_io_fatal(void);
static void wait_for_interrupt(void);

/*
 * Reports the processor's record.
 */
struct ppc64_percpu *
ppc64_percpu_current(
	void)
{
	/* The port is uniprocessor. */
	return &boot_cpu;
}

/*
 * Waits for an interrupt, takes it, and leaves with interrupts off.
 */
void
hal_cpu_idle(
	void)
{
	/* Waits with interrupts on; the interrupt is taken before the return. */
	wait_for_interrupt();
	ppc64_write_msr(ppc64_read_msr() & ~(uint64_t)PPC64_MSR_EE);
}

/*
 * Waits until the next interrupt when interrupts are on.
 */
void
hal_halt(
	void)
{
	bool masked;

	/* With interrupts off nothing can wake it, so it only spins once. */
	masked = ppc64_irq_masked();
	if (masked) {
		__asm__ volatile("" : : : "memory");
		return;
	}

	/* Otherwise it waits for the next interrupt. */
	wait_for_interrupt();
}

/*
 * Writes cached lines of a range back to memory.
 */
void
hal_dcache_clean_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;

	/* Each line of the range, then the ordering. */
	for (line = address & ~(uintptr_t)127U; line < address + size; line += 128U)
		__asm__ volatile("dcbst 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Discards cached lines of a range; they are written back first, which
 * keeps the data of lines the range shares.
 */
void
hal_dcache_invalidate_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;

	/* Each line of the range, then the ordering. */
	for (line = address & ~(uintptr_t)127U; line < address + size; line += 128U)
		__asm__ volatile("dcbf 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Writes back and discards cached lines of a range.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t address,
	size_t size)
{
	/* The same as the discard. */
	hal_dcache_invalidate_range(address, size);
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
	ppc64_halt_forever();
}

/* Stops the kernel at a port access. */
static void
port_io_fatal(
	void)
{
	/* Port I/O has no meaning on this architecture. */
	HAL_FATAL("port I/O is unavailable on ppc64");
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
	ppc64_halt_forever();
}

/*
 * Stops every processor after a panic: this one.
 */
_Noreturn void
hal_cpu_panic_all(
	void)
{
	/* The same stop. */
	ppc64_halt_forever();
}

/*
 * Waits for an interrupt with interrupts on.  In a partition the
 * hypervisor waits and returns with the interrupt pending, which is taken
 * as interrupts come on; a processor run directly only opens the window
 * for one already pending, and the caller's loop comes back.
 */
static void
wait_for_interrupt(
	void)
{
	const struct ppc64_board_info *board;
	uint64_t arguments[8];
	uint64_t state;

	/* The hypervisor's wait, in a partition. */
	board = ppc64_board();
	if (board->under_hypervisor) {
		hal_memset(arguments, 0, sizeof(arguments));
		(void)ppc64_hcall(PPC64_H_CEDE, arguments);
		return;
	}

	/* A window with interrupts on, closed again as it was. */
	state = ppc64_read_msr();
	ppc64_write_msr(state | PPC64_MSR_EE);
	ppc64_write_msr(state);
}
