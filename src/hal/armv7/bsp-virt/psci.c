/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's firmware calls: PSCI's system reset and system off,
 * through the conduit the device tree names.  The emulator answers them
 * itself, through a hypervisor call by default.
 *
 * Values referenced: ARM DEN 0022 (Power State Coordination Interface),
 * the function identifiers SYSTEM_OFF 0x84000008 and SYSTEM_RESET
 * 0x84000009 of the 32-bit calling convention.
 */

#include <hal/hal.h>
#include "virt.h"

/* The function identifiers. */
#define PSCI_SYSTEM_OFF		0x84000008U
#define PSCI_SYSTEM_RESET	0x84000009U

/*
 * The conduit, ARMV7_FDT_PSCI_*.  Set once on the boot processor from the
 * device tree; ARMV7_FDT_PSCI_NONE when the tree names none.
 */
static unsigned psci_method;

static void psci_call(uint32_t function);

/*
 * Records the conduit the device tree names.
 */
void
virt_psci_init(
	const struct armv7_fdt_info *info)
{
	/* Keeps the conduit for the calls. */
	psci_method = info->psci_method;
}

/*
 * Resets the machine; returns only when the firmware cannot.
 */
void
virt_psci_system_reset(
	void)
{
	/* The firmware resets and does not return. */
	psci_call(PSCI_SYSTEM_RESET);
}

/*
 * Turns the machine off; returns only when the firmware cannot.
 */
void
virt_psci_system_off(
	void)
{
	/* The firmware turns the machine off and does not return. */
	psci_call(PSCI_SYSTEM_OFF);
}

/* Makes one firmware call with no arguments through the conduit. */
static void
psci_call(
	uint32_t function)
{
	register uint32_t function_register __asm__("r0");

	/* The function identifier goes in r0 for either conduit. */
	function_register = function;

	/* The hypervisor call, which the virtualization extensions name. */
	if (psci_method == ARMV7_FDT_PSCI_HVC) {
		__asm__ volatile(".arch_extension virt\n\thvc #0"
				 : "+r"(function_register)
				 :
				 : "r1", "r2", "r3", "memory");
		return;
	}

	/* The secure monitor call, which the security extensions name. */
	if (psci_method == ARMV7_FDT_PSCI_SMC) {
		__asm__ volatile(".arch_extension sec\n\tsmc #0"
				 : "+r"(function_register)
				 :
				 : "r1", "r2", "r3", "memory");
	}
}
