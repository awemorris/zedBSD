/* amd64 HAL bring-up bridge. */
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

void
amd64_cmain(const void *raw_boot_info)
{
	static struct amd64_acpi_info acpi;

	bsp_boot_init(raw_boot_info);
	pcat_cons_init();
	amd64_cpu_init();
	amd64_percpu_bootstrap();
	hal_puts("\nzedBSD amd64 HAL\n");
	hal_puts("A64 ENTRY PASS\n");
	amd64_page_init();
	amd64_space_init();
	hal_puts("A64 PAGING PASS\n");
	amd64_descriptor_init();
	amd64_int_init();
	hal_puts("A64 IDT READY\n");
	if (amd64_acpi_discover(&acpi, bsp_acpi_rsdp()) != HAL_OK)
		HAL_FATAL("amd64 ACPI MADT discovery failed");
	if (amd64_lapic_init(&acpi) != HAL_OK)
		HAL_FATAL("amd64 Local APIC initialization failed");
	amd64_smp_init(&acpi);
	irq_init(&acpi);
	if (bsp_timer_init() != HAL_OK)
		HAL_FATAL("amd64 Local APIC timer initialization failed");
	pcat_cons_irq_init();
	hal_puts("A64 CONSOLE IRQ READY\n");
	hal_puts("A64 IRQ READY\n");
	kernel_entry(bsp_kernel_handoff(raw_boot_info));
	HAL_FATAL("amd64 kernel_entry returned");
}
