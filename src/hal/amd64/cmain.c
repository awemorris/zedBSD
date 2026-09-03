/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 HAL bootstrap entry point.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "space.h"
#include "descriptor.h"
#include "irq.h"
#include "clock.h"
#include "percpu.h"
#include "smp.h"
#include "bsp-pcat/acpi.h"
#include "bsp-pcat/lapic.h"

void amd64_page_init(void);
void amd64_int_init(void);
void kernel_entry(const void *handoff);

/*
 * Initializes the amd64 HAL and enters the kernel.
 */
void
amd64_cmain(
	const void *raw_boot_info)
{
	static struct amd64_acpi_info acpi;
	const void *handoff;
	hal_physaddr_t rsdp_address;
	int error;

	/* Establishes boot state, console output, and CPU-local facilities. */
	bsp_boot_init(raw_boot_info);
	pcat_cons_init();
	amd64_cpu_init();
	amd64_percpu_bootstrap();
	hal_puts("\nzedBSD amd64 HAL\n");
	hal_puts("A64 ENTRY PASS\n");

	/* Establishes kernel paging and address-space management. */
	amd64_page_init();
	amd64_space_init();
	hal_puts("A64 PAGING PASS\n");

	/* Installs the BSP descriptor and interrupt tables. */
	amd64_descriptor_init();
	amd64_int_init();
	hal_puts("A64 IDT READY\n");

	/* Discovers the platform interrupt topology from ACPI. */
	rsdp_address = bsp_acpi_rsdp();
	error = amd64_acpi_discover(&acpi, rsdp_address);
	if (error != HAL_OK)
		HAL_FATAL("amd64 ACPI MADT discovery failed");

	/* Enables the BSP local APIC before bringing up secondary CPUs. */
	error = amd64_lapic_init(&acpi);
	if (error != HAL_OK)
		HAL_FATAL("amd64 Local APIC initialization failed");
	amd64_smp_init(&acpi);

	/* Enables external interrupts, the scheduler clock, and console input. */
	irq_init(&acpi);
	error = bsp_timer_init();
	if (error != HAL_OK)
		HAL_FATAL("amd64 Local APIC timer initialization failed");
	pcat_cons_irq_init();
	hal_puts("A64 CONSOLE IRQ READY\n");
	hal_puts("A64 IRQ READY\n");

	/* Converts the board handoff and transfers control to the kernel. */
	handoff = bsp_kernel_handoff(raw_boot_info);
	kernel_entry(handoff);

	/* Treats an unexpected return from the kernel as fatal. */
	HAL_FATAL("amd64 kernel_entry returned");
}
