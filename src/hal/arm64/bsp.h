#ifndef ZEDBSD_HAL_ARM64_BSP_H
#define ZEDBSD_HAL_ARM64_BSP_H

#include "bsp-rpi4/fdt.h"

void rpi4_boot_set_info(const struct rpi4_fdt_info *info, uintptr_t fdt_phys);
void rpi4_boot_set_framebuffer(uint64_t phys,uint64_t size,uint32_t width,
	uint32_t height,uint32_t pitch,uint32_t format);
const struct rpi4_fdt_info *rpi4_boot_info(void);
uintptr_t rpi4_boot_fdt_phys(void);
const void *rpi4_kernel_handoff(void);
void rpi4_cons_init(void);
void rpi4_cons_irq_init(void);

#endif
