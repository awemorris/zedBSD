/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC processor record, the processor's family, the idle
 * and halt, the barriers and the memory-mapped I/O accessors.
 *
 * sync orders every access; eieio orders device accesses, which the
 * uncached, guarded windows do not reorder further.
 *
 * The family is the PVR's version.  The idle wait sets MSR[POW] with the
 * power-saving state HID0 selects: doze on the 603, 750 and 7400, which
 * the 7450 family does not have; nap there.  HID0's doze bit is the high
 * BAT enable on the 7455 and later, so it is never set on them for
 * idling.  The time base and the decrementer run in both states, so the
 * tick ends the wait.  The kernel issues no AltiVec data stream, so no
 * stream needs stopping before nap.
 *
 * The 7450 family can hang entering nap while its L2 hardware prefetch
 * engines are busy, so the wait turns the prefetch off (MSSCR0's two
 * low bits), flushes a line of the image a few times to let the engines
 * drain, naps, and turns the prefetch back on after the interrupt.
 * Approach referenced: Linux arch/powerpc/kernel/idle_6xx.S, the
 * pre-nap step for the 745x, which follows the MPC7450 chip errata.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "percpu.h"

/*
 * The PVR's version field, and the 7450 family's versions; HID0's
 * power-saving states; MSR[POW].
 *
 * Values referenced: MPC7450 RISC Microprocessor Family Reference Manual
 * (MPC7450UM): the PVR's version numbers of the 7450 (0x8000), 7455
 * (0x8001), 7457 (0x8002), 7447A (0x8003) and 7448 (0x8004), HID0's nap
 * (bit 9 from the most significant end) and sleep (bit 10) and the
 * power-management chapter (nap entered by setting MSR[POW] with
 * HID0[NAP] set, sync before and isync after); the high BATs of the
 * 7455 and later.  PowerPC 750 and MPC7400 user's manuals: HID0's doze,
 * nap and sleep (bits 8 to 10).
 */
#define PVR_VERSION_SHIFT	16
#define PVR_7450		0x8000U
#define PVR_7448		0x8004U

/* HID0's power-saving states: doze (not on the 7450 family), nap and sleep. */
#define HID0_DOZE		0x00800000U
#define HID0_NAP		0x00400000U
#define HID0_SLEEP		0x00200000U

/* MSR[POW], which enters the power-saving state HID0 selects. */
#define MSR_POW			0x00040000U

/* MSSCR0's L2 prefetch engine count on the 7450 family, its two least significant bits. */
#define MSSCR0_L2PFE		0x00000003U

/* The one processor's record. */
static struct ppc32_percpu boot_cpu;

/* The processor's PVR, and whether it is of the 7450 family; set once by ppc32_cpu_identify(). */
static uint32_t cpu_pvr;
static bool cpu_7450_family;

/*
 * The MSR bits the idle wait sets besides EE: MSR_POW when the processor
 * saves power while it waits, 0 when it only waits.  Read by
 * ppc32_cpu_idle(); set once by ppc32_cpu_power_init().
 */
uint32_t ppc32_idle_power;

/*
 * The MSSCR0 bits the idle wait clears while it naps: the L2 prefetch
 * engines on the 7450 family, 0 when it does not nap or on the others.
 * Read by ppc32_cpu_idle(); set once by ppc32_cpu_power_init().
 */
uint32_t ppc32_idle_prefetch;

static void port_io_fatal(void);

/*
 * Reports the processor's record.
 */
struct ppc32_percpu *
ppc32_percpu_current(
	void)
{
	/* The port is uniprocessor. */
	return &boot_cpu;
}

/*
 * Reads the processor's version and says which it is.
 */
void
ppc32_cpu_identify(
	void)
{
	uint32_t version;
	const char *family;
	unsigned pairs;
	bool high;

	/* The PVR names the family. */
	__asm__ volatile("mfpvr %0" : "=r"(cpu_pvr));
	version = cpu_pvr >> PVR_VERSION_SHIFT;
	cpu_7450_family = false;
	if (version >= PVR_7450 && version <= PVR_7448)
		cpu_7450_family = true;

	/* Says what the HAL found. */
	family = "6XX";
	if (cpu_7450_family)
		family = "7450-FAMILY";
	pairs = PPC32_BAT_PAIRS;
	high = ppc32_cpu_has_high_bats();
	if (high)
		pairs = PPC32_BAT_PAIRS_HIGH;
	hal_printf("PPC32 CPU PVR %08x %s BATS %u\n", cpu_pvr, family, pairs);
}

/*
 * Tells whether the processor has the four high block translation pairs:
 * the 7455 and the later 7450-family processors, not the 7450 itself.
 */
bool
ppc32_cpu_has_high_bats(
	void)
{
	/* The 7450 family after the 7450. */
	if (!cpu_7450_family)
		return false;
	if ((cpu_pvr >> PVR_VERSION_SHIFT) == PVR_7450)
		return false;
	return true;
}

/*
 * Chooses the idle wait's power-saving state in HID0: nap on the 7450
 * family, doze on the others; none when asked not to save power.
 */
void
ppc32_cpu_power_init(
	bool save_power)
{
	uint32_t hid0;
	const char *state;

	/* Clears the states this processor has, keeping every other bit (the high BAT enable among them). */
	__asm__ volatile("mfspr %0,1008" : "=r"(hid0));
	if (cpu_7450_family) {
		hid0 &= ~(HID0_NAP | HID0_SLEEP);
	} else {
		hid0 &= ~(HID0_DOZE | HID0_NAP | HID0_SLEEP);
	}

	/* The state the wait enters. */
	state = "OFF";
	ppc32_idle_power = 0;
	ppc32_idle_prefetch = 0;
	if (save_power) {
		ppc32_idle_power = MSR_POW;
		if (cpu_7450_family) {
			hid0 |= HID0_NAP;
			ppc32_idle_prefetch = MSSCR0_L2PFE;
			state = "NAP";
		} else {
			hid0 |= HID0_DOZE;
			state = "DOZE";
		}
	}
	__asm__ volatile("sync\n\tmtspr 1008,%0\n\tisync" : : "r"(hid0) : "memory");
	hal_printf("PPC32 IDLE %s HID0 %08x\n", state, hid0);
}

/*
 * Waits for an interrupt, takes it, and leaves with interrupts off.
 */
void
hal_cpu_idle(
	void)
{
	/* The wait opens the mask; the interrupt is taken before the return. */
	ppc32_cpu_idle();
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
	masked = ppc32_irq_masked();
	if (masked) {
		__asm__ volatile("" : : : "memory");
		return;
	}

	/* Waits for the next interrupt with interrupts on. */
	ppc32_cpu_idle();
	ppc32_write_msr(ppc32_read_msr() | PPC32_MSR_EE);
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
	ppc32_halt_forever();
}

/* Stops the kernel at a port access. */
static void
port_io_fatal(
	void)
{
	/* Port I/O has no meaning on this architecture. */
	HAL_FATAL("port I/O is unavailable on ppc32");
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
	ppc32_halt_forever();
}

/*
 * Stops every processor after a panic: this one.
 */
_Noreturn void
hal_cpu_panic_all(
	void)
{
	/* The same stop. */
	ppc32_halt_forever();
}
