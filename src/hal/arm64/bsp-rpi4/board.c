/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the Raspberry Pi 4 gives the arm64 HAL beyond the Raspberry Pi
 * common support: its peripherals in the kernel's tables, the release of
 * its cores from the firmware's spin table, and the watchdog reset.  Its
 * compositor for the stall watch is watch-hvs.c.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../bsp.h"
#include "../defs.h"
#include "../space.h"
#include "../watch.h"
#include "board.h"

/*
 * The power manager's watchdog: the reset control and the watchdog
 * registers, the password every write carries, and the values that start
 * a full reset after ten ticks.
 */
#define PM_RSTC 0x1cU
#define PM_WDOG 0x24U
#define PM_PASSWORD 0x5a000000U
#define PM_WDOG_TICKS 0x0000000aU
#define PM_RSTC_FULL_RESET 0x00000020U


/*
 * Maps the peripherals the HAL itself uses into the kernel's tables: each
 * gets a 2 MiB device block in the 3-4 GiB window.
 */
void
arm64_bsp_map_devices(
	void)
{
	const struct arm64_fdt_info *info;

	/* The device tree names where each peripheral is. */
	info = arm64_boot_info();

	/* The UART, the mailbox, the GIC's two parts and the SD controller. */
	arm64_space_map_device_block(info->uart_base);
	arm64_space_map_device_block(info->mailbox_base);
	arm64_space_map_device_block(info->gic_dist_base);
	arm64_space_map_device_block(info->gic_cpu_base);
	arm64_space_map_device_block(info->sdhci_base);
}

/*
 * Reports a core's number: the Cortex-A72 numbers its cores in Aff0.
 */
uint64_t
arm64_bsp_core_of(
	uint64_t mpidr)
{
	/* Succeeded: the lowest affinity field. */
	return mpidr & 0xffU;
}

/*
 * Tells whether a core waits in the firmware's spin table, which is how
 * the Pi 4's firmware holds its cores.
 */
int
arm64_bsp_cpu_startable(
	const struct arm64_fdt_cpu *cpu)
{
	/* A core without a spin table, or without the address it waits on, cannot be released. */
	if (cpu->spin_table == 0)
		return 0;
	if (cpu->release_addr == 0)
		return 0;

	/* Succeeded: the core can be released. */
	return 1;
}

/*
 * Releases a core from its spin table: writes the entry where it waits,
 * and wakes it from its wait for an event.
 */
int
arm64_bsp_cpu_release(
	const struct arm64_fdt_cpu *cpu,
	uint64_t entry_phys)
{
	volatile uint64_t *release;

	/* Writes the entry's physical address where the core waits, out to memory, which it reads uncached. */
	release = arm64_phys_to_direct((uintptr_t)cpu->release_addr);
	*release = entry_phys;
	hal_dcache_clean_invalidate_range((uintptr_t)release, sizeof(*release));

	/* Wakes the core, after the write has reached memory. */
	__asm__ volatile("dsb sy\n\tsev" : : : "memory");

	/* Succeeded: the core goes to the entry. */
	return HAL_OK;
}

/*
 * Resets the board through the power manager's watchdog.
 */
void
hal_reset(
	void)
{
	volatile uint8_t *pm;

	/* Nothing interrupts the reset. */
	arm64_irq_mask();

	/* Starts the watchdog and asks for a full reset when it expires. */
	pm = (volatile uint8_t *)(ARM64_DIRECT_BASE + RPI4_PM_BASE);
	hal_mmio_write32(pm + PM_WDOG, PM_PASSWORD | PM_WDOG_TICKS);
	hal_mmio_write32(pm + PM_RSTC, PM_PASSWORD | PM_RSTC_FULL_RESET);

	/* Waits for the reset. */
	for (;;)
		arm64_wfi();
}

/*
 * Stops the calling CPU for good when the board is to be powered off.
 *
 * The board has no power switch the kernel can reach, so the CPU halts
 * with interrupts off; the stall watch is told that its silence is meant.
 */
void
hal_poweroff(
	void)
{
	/* Nothing wakes the CPU into the kernel again. */
	arm64_irq_mask();

	/* The other CPUs do not report this one as stalled. */
	arm64_watch_park();

	/* Halts for ever. */
	for (;;)
		arm64_wfi();
}
