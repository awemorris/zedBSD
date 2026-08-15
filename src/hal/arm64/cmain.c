#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "bsp-rpi4/fdt.h"
#include "bsp.h"
#include "space.h"
#include "int.h"
#include "task.h"
#include "bsp-rpi4/framebuffer.h"

void
arm64_cmain(uintptr_t fdt_phys)
{
	const void *fdt;
	struct rpi4_fdt_info info;
	int error;

	rpi4_cons_init();
	hal_puts("RPI4 ENTRY\n");
	if (arm64_current_el() != 1)
		HAL_FATAL("AArch64 kernel did not enter EL1");
	hal_puts("RPI4 EL1 PASS\n");
	fdt = (const void *)(ARM64_DIRECT_BASE + fdt_phys);
	if (fdt_phys == 0)
		HAL_FATAL("missing firmware FDT");
	error = rpi4_fdt_parse(fdt, 2U * 1024U * 1024U, &info);
	if (error != 0) {
		hal_printf("FDT parse error: %s\n", rpi4_fdt_error(error));
		HAL_FATAL("invalid firmware FDT");
	}
	hal_printf("RPI4 FDT PASS phys=%p size=%u\n", (void *)fdt_phys, info.totalsize);
	hal_printf("RPI4 MMIO uart=%llx gic=%llx/%llx sd=%llx\n",
	    info.uart_base, info.gic_dist_base, info.gic_cpu_base, info.sdhci_base);
	rpi4_boot_set_info(&info, fdt_phys);
	if(rpi4_framebuffer_init(info.mailbox_base)==0)
		hal_puts("RPI4 FRAMEBUFFER PASS\n");
	else
		hal_puts("RPI4 framebuffer unavailable; UART-only console\n");
	arm64_page_init();
	arm64_space_init();
	arm64_context_selftest();
	arm64_int_init();
	hal_irq_enable();
	kernel_entry(rpi4_kernel_handoff());
	for (;;) arm64_wfi();
}
