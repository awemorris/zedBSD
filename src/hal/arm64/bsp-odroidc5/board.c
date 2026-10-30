/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ODROID-C5's start-up beyond what the S905X5M boards share
 * (bsp-s905x5m-common/): the peripherals the HAL uses, in the kernel's
 * tables.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "board.h"

/* The bytes of one HAL peripheral window: a page covers each register block. */
#define ODROIDC5_DEVICE_WINDOW	0x1000UL

static void map_one(uint64_t physical, const char *name);

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
	const struct arm64_fdt_info *info;

	/* The debug UART the early console writes to before the tree is read. */
	map_one(S905X5M_DEBUG_UART_BASE, "debug UART");
	info = arm64_boot_info();

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
				     ODROIDC5_DEVICE_WINDOW,
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
