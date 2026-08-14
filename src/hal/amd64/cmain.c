/* amd64 HAL bring-up bridge. */
#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "space.h"
#include "descriptor.h"
#include "irq.h"
#include "clock.h"

void amd64_page_init(void);
void amd64_int_init(void);
void kernel_entry(const void *handoff);

void
amd64_cmain(const void *raw_boot_info)
{
	bsp_boot_init(raw_boot_info);
	bsp_cons_init();
	amd64_cpu_init();
	hal_puts("\nzedBSD amd64 HAL\n");
	hal_puts("A64 ENTRY PASS\n");
	amd64_page_init();
	amd64_space_init();
	hal_puts("A64 PAGING PASS\n");
	amd64_descriptor_init();
	amd64_int_init();
	irq_init();
	bsp_timer_init();
	hal_puts("A64 IRQ READY\n");
	kernel_entry(bsp_kernel_handoff(raw_boot_info));
	HAL_FATAL("amd64 kernel_entry returned");
}
