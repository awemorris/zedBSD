/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the ODROID-C5 gives the arm64 HAL beyond the common support: its
 * peripherals in the kernel's tables, and the secure firmware's PSCI
 * calls that start the cores, reset and power off.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../bsp.h"
#include "../defs.h"
#include "../watch.h"
#include "board.h"

/*
 * The PSCI functions the HAL calls: SYSTEM_OFF and SYSTEM_RESET in the
 * SMC32 numbering every PSCI version keeps, and CPU_ON in the SMC64 one,
 * whose entry address is 64 bits wide.
 *
 * Values referenced: Arm Power State Coordination Interface (DEN0022),
 * the function identifiers of SYSTEM_OFF, SYSTEM_RESET and CPU_ON.
 */
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_CPU_ON_64		0xc4000003UL

/*
 * The PSCI results CPU_ON reports.
 *
 * Values referenced: Arm Power State Coordination Interface (DEN0022),
 * the return codes SUCCESS and ALREADY_ON.
 */
#define PSCI_SUCCESS		0
#define PSCI_ALREADY_ON		(-4)

/* The affinity field the Cortex-A55 numbers its cores in: Aff1 (MPIDR bits 8-15), since MT is set. */
#define CORE_AFFINITY_SHIFT	8U

/* The bytes of one HAL peripheral window: a page covers each register block. */
#define S905X5M_DEVICE_WINDOW	0x1000UL

static void map_one(uint64_t physical, const char *name);
static int64_t psci_call(uint64_t function, uint64_t argument1, uint64_t argument2, uint64_t argument3);

/*
 * Maps the peripherals the HAL itself uses into the kernel's direct map.
 *
 * Each one gets a device window of its own; the rest of the gigabyte the
 * boot mapped as device memory is RAM's or nobody's in the kernel's
 * tables.  The tables are not live yet; a failure stops the boot, since
 * the console and the interrupt controller would be gone after the
 * switch.
 */
void
arm64_bsp_map_devices(
	void)
{
	const struct rpi_fdt_info *info;

	/* The debug UART the early console writes to before the tree is read. */
	map_one(S905X5M_DEBUG_UART_BASE, "debug UART");
	info = rpi_boot_info();

	/* The GIC's distributor, which routes the shared interrupts. */
	if (info->gic_dist_base != 0)
		map_one(info->gic_dist_base, "GIC distributor");

	/* The GIC's CPU interface, which every interrupt is taken through. */
	if (info->gic_cpu_base != 0)
		map_one(info->gic_cpu_base, "GIC CPU interface");

	/* The UART the tree names, in case it is not the fixed debug UART. */
	if (info->uart_base != 0 && info->uart_base != S905X5M_DEBUG_UART_BASE)
		map_one(info->uart_base, "UART");
}

/*
 * Leaves the stall watch's report on the framebuffer.
 *
 * The watch knows only the Pi 4's compositor, so the board offers none.
 */
volatile uint32_t *
arm64_bsp_watch_compositor(
	void)
{
	/* Succeeded: no compositor the watch knows. */
	return NULL;
}

/*
 * Reports a core's number: the Cortex-A55 numbers its cores in Aff1.
 */
uint64_t
arm64_bsp_core_of(
	uint64_t mpidr)
{
	uint64_t core;

	/* The second affinity field. */
	core = (mpidr >> CORE_AFFINITY_SHIFT) & 0xffU;

	/* Succeeded: the core's number. */
	return core;
}

/*
 * Tells whether a core is started through PSCI, which is how the
 * S905X5M's secure firmware (Amlogic's BL31) holds its cores.
 */
int
arm64_bsp_cpu_startable(
	const struct rpi_fdt_cpu *cpu)
{
	/* A core the tree does not give to PSCI is left alone. */
	if (cpu->psci == 0)
		return 0;

	/* Succeeded: the core can be started. */
	return 1;
}

/*
 * Starts a core through PSCI CPU_ON at a physical entry point.
 *
 * The secure firmware brings the core up at EL2 with its MMU off; the
 * context value it passes in x0 is not used.
 */
int
arm64_bsp_cpu_release(
	const struct rpi_fdt_cpu *cpu,
	uint64_t entry_phys)
{
	int64_t result;

	/* Asks the secure firmware to start the core named by its affinity. */
	result = psci_call(PSCI_CPU_ON_64, cpu->mpidr, entry_phys, 0);

	/* A core already on did not take the entry; it is not what the HAL started. */
	if (result == PSCI_ALREADY_ON)
		return HAL_ERR_STATE;

	/* Any other refusal (a bad affinity or entry, a denied request) starts nothing. */
	if (result != PSCI_SUCCESS)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the core goes to the entry. */
	return HAL_OK;
}

/*
 * Resets the board through the firmware.
 */
void
hal_reset(
	void)
{
	/* Nothing interrupts the reset request. */
	arm64_irq_mask();

	/* Asks the secure firmware to reset the system; it does not return. */
	(void)psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);

	/* A firmware that refused leaves the CPU halted. */
	arm64_watch_park();
	for (;;)
		arm64_wfi();
}

/*
 * Turns the board off through the firmware.
 */
void
hal_poweroff(
	void)
{
	/* Nothing interrupts the power-off request. */
	arm64_irq_mask();

	/* The other CPUs do not report this one as stalled while it waits. */
	arm64_watch_park();

	/* Asks the secure firmware to turn the system off; it does not return. */
	(void)psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);

	/* A firmware that refused leaves the CPU halted. */
	for (;;)
		arm64_wfi();
}

/* Maps one peripheral window as device memory, or stops the boot. */
static void
map_one(
	uint64_t physical,
	const char *name)
{
	void *window;
	int error;

	/* Gives the window a Device-nGnRE block in the direct map. */
	error = hal_space_map_device((hal_physaddr_t)physical,
				     S905X5M_DEVICE_WINDOW,
				     HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_DEVICE,
				     &window);
	if (error != HAL_OK) {
		hal_printf(ARM64_BOARD_TAG " cannot map the %s at %llx (%d)\n",
			   name,
			   (unsigned long long)physical,
			   error);
		HAL_FATAL("ODROID-C5 peripheral mapping failed");
	}
}

/* Makes one PSCI call through the secure monitor, and reports its result. */
static int64_t
psci_call(
	uint64_t function,
	uint64_t argument1,
	uint64_t argument2,
	uint64_t argument3)
{
	register uint64_t x0 __asm__("x0");
	register uint64_t x1 __asm__("x1");
	register uint64_t x2 __asm__("x2");
	register uint64_t x3 __asm__("x3");

	/*
	 * The function identifier and its arguments go in x0 to x3, and the
	 * result comes back in x0; the SMC calling convention may clobber x4
	 * to x17.
	 */
	x0 = function;
	x1 = argument1;
	x2 = argument2;
	x3 = argument3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9",
			   "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
			   "memory");

	/* Succeeded: the firmware's result, a PSCI return code. */
	return (int64_t)x0;
}
